/**
 * @file ws.c
 * @brief WebSocket message queue and connection map.
 */
#define _POSIX_C_SOURCE 200809L

#include "gateway/ws.h"
#ifdef SHELLCLAW_WS_TEST
struct lws;
struct lws_context;
static void lws_callback_on_writable(struct lws *wsi) { (void)wsi; }
#else
#include "gateway/lws_compat.h"
#include <libwebsockets.h>
#endif
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <time.h>
#include <unistd.h>

#define MAX_CONNECTIONS 16
#define MAX_QUEUE 64
#define MSG_MAX 8192

static size_t ws_text_len_ok(const char *text)
{
	size_t len;
	if (!text) return 0;
	len = strlen(text);
	return len > 0 && len <= (size_t)MSG_MAX;
}

typedef struct ws_msg {
	int conn_id;
	char *text;
	struct ws_msg *next;
} ws_msg_t;

typedef struct {
	ws_conn_t wsi;
	int conn_id;
} ws_conn_entry_t;

static ws_conn_entry_t g_conns[MAX_CONNECTIONS];
static ws_msg_t *g_queue_head;
static ws_msg_t *g_queue_tail;
static ws_msg_t *g_outgoing_head;
static ws_msg_t *g_outgoing_tail;
static int g_incoming_count;
static int g_outgoing_count;
static pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cond = PTHREAD_COND_INITIALIZER;
static atomic_int g_next_conn_id;
static volatile int g_ws_shutdown;
static struct lws_context *g_lws_ctx;

void ws_set_context(void *ctx)
{
	g_lws_ctx = (struct lws_context *)ctx;
}

int ws_next_conn_id(void)
{
	return atomic_fetch_add(&g_next_conn_id, 1) + 1;
}

int ws_register_conn(int conn_id, ws_conn_t wsi)
{
	int registered = 0;
	pthread_mutex_lock(&g_mutex);
	for (int i = 0; i < MAX_CONNECTIONS; i++) {
		if (g_conns[i].wsi == NULL) {
			g_conns[i].wsi = wsi;
			g_conns[i].conn_id = conn_id;
			registered = 1;
			break;
		}
	}
	pthread_mutex_unlock(&g_mutex);
	return registered ? 0 : -1;
}

void ws_unregister_conn(int conn_id)
{
	pthread_mutex_lock(&g_mutex);
	for (int i = 0; i < MAX_CONNECTIONS; i++) {
		if (g_conns[i].conn_id == conn_id) {
			g_conns[i].wsi = NULL;
			g_conns[i].conn_id = 0;
			break;
		}
	}
	pthread_mutex_unlock(&g_mutex);
}

void ws_push_incoming(int conn_id, const char *text)
{
	if (!ws_text_len_ok(text)) return;
	ws_msg_t *m = malloc(sizeof(*m));
	if (!m) return;
	m->conn_id = conn_id;
	m->text = strdup(text);
	m->next = NULL;
	if (!m->text) { free(m); return; }
	pthread_mutex_lock(&g_mutex);
	if (g_incoming_count >= MAX_QUEUE) {
		pthread_mutex_unlock(&g_mutex);
		free(m->text);
		free(m);
		return;
	}
	if (!g_queue_tail)
		g_queue_head = g_queue_tail = m;
	else {
		g_queue_tail->next = m;
		g_queue_tail = m;
	}
	g_incoming_count++;
	pthread_cond_signal(&g_cond);
	pthread_mutex_unlock(&g_mutex);
}

static int parse_conn_id(const char *session_id)
{
	if (!session_id || strncmp(session_id, "webchat:", 8) != 0) return -1;
	return atoi(session_id + 8);
}

int ws_pop_incoming(char *session_id_out, size_t session_size,
                    char *text_out, size_t text_size, int timeout_ms)
{
	if (!session_id_out || session_size == 0 || !text_out || text_size == 0) return -1;
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	ts.tv_sec += timeout_ms / 1000;
	ts.tv_nsec += (timeout_ms % 1000) * 1000000;
	if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
	pthread_mutex_lock(&g_mutex);
	while (!g_queue_head && !g_ws_shutdown) {
		int r = pthread_cond_timedwait(&g_cond, &g_mutex, &ts);
		if (r == ETIMEDOUT) {
			pthread_mutex_unlock(&g_mutex);
			return 0;
		}
	}
	if (g_ws_shutdown) {
		pthread_mutex_unlock(&g_mutex);
		return 0;
	}
	ws_msg_t *m = g_queue_head;
	g_queue_head = m->next;
	if (!g_queue_head) g_queue_tail = NULL;
	g_incoming_count--;
	pthread_mutex_unlock(&g_mutex);
	snprintf(session_id_out, session_size, "webchat:%d", m->conn_id);
	size_t len = strlen(m->text);
	if (len >= text_size) len = text_size - 1;
	memcpy(text_out, m->text, len);
	text_out[len] = '\0';
	free(m->text);
	free(m);
	return 1;
}

/**
 * Dequeue next outgoing message for conn_id. Call from lws thread only.
 * Returns 1 and fills buf if message found, 0 if none. Caller must not hold g_mutex.
 */
int ws_dequeue_outgoing(int conn_id, char *buf, size_t buf_size, size_t *len_out)
{
	if (!buf || buf_size == 0 || !len_out) return 0;
	ws_msg_t *prev = NULL;
	ws_msg_t *m = NULL;
	pthread_mutex_lock(&g_mutex);
	ws_msg_t *p = g_outgoing_head;
	while (p) {
		if (p->conn_id == conn_id) {
			m = p;
			if (prev)
				prev->next = p->next;
			else
				g_outgoing_head = p->next;
			if (g_outgoing_tail == p)
				g_outgoing_tail = prev;
			g_outgoing_count--;
			break;
		}
		prev = p;
		p = p->next;
	}
	pthread_mutex_unlock(&g_mutex);
	if (!m) return 0;
	size_t len = strlen(m->text);
	if (len >= buf_size) len = buf_size - 1;
	memcpy(buf, m->text, len + 1);
	*len_out = len;
	free(m->text);
	free(m);
	return 1;
}

/**
 * Check if more outgoing messages pending for conn_id. Call from lws thread only.
 */
int ws_has_pending_outgoing(int conn_id)
{
	int found = 0;
	pthread_mutex_lock(&g_mutex);
	for (ws_msg_t *p = g_outgoing_head; p; p = p->next) {
		if (p->conn_id == conn_id) { found = 1; break; }
	}
	pthread_mutex_unlock(&g_mutex);
	return found;
}

int ws_send_to(const char *session_id, const char *text)
{
	if (!session_id || !ws_text_len_ok(text)) return -1;
	int conn_id = parse_conn_id(session_id);
	if (conn_id < 0) return -1;
	ws_msg_t *m = malloc(sizeof(*m));
	if (!m) return -1;
	m->conn_id = conn_id;
	m->text = strdup(text);
	m->next = NULL;
	if (!m->text) { free(m); return -1; }
	pthread_mutex_lock(&g_mutex);
	if (g_outgoing_count >= MAX_QUEUE) {
		pthread_mutex_unlock(&g_mutex);
		free(m->text);
		free(m);
		return -1;
	}
	if (!g_outgoing_tail)
		g_outgoing_head = g_outgoing_tail = m;
	else {
		g_outgoing_tail->next = m;
		g_outgoing_tail = m;
	}
	g_outgoing_count++;
	ws_conn_t wsi = NULL;
	for (int i = 0; i < MAX_CONNECTIONS; i++) {
		if (g_conns[i].conn_id == conn_id) {
			wsi = g_conns[i].wsi;
			break;
		}
	}
	pthread_mutex_unlock(&g_mutex);
	if (!wsi) return 0;
	lws_callback_on_writable((struct lws *)wsi);
	return 0;
}

void ws_broadcast_text(const char *text)
{
	int i;
	int wake_count = 0;
	ws_conn_t wake_list[MAX_CONNECTIONS];
	if (!ws_text_len_ok(text)) return;
	pthread_mutex_lock(&g_mutex);
	for (i = 0; i < MAX_CONNECTIONS; i++) {
		ws_msg_t *m;
		int cid;
		if (!g_conns[i].wsi) continue;
		cid = g_conns[i].conn_id;
		if (g_outgoing_count >= MAX_QUEUE)
			break;
		m = malloc(sizeof(*m));
		if (!m) break;
		m->conn_id = cid;
		m->text = strdup(text);
		if (!m->text) {
			free(m);
			break;
		}
		m->next = NULL;
		if (!g_outgoing_tail)
			g_outgoing_head = g_outgoing_tail = m;
		else {
			g_outgoing_tail->next = m;
			g_outgoing_tail = m;
		}
		g_outgoing_count++;
		if (wake_count < MAX_CONNECTIONS) {
			wake_list[wake_count] = g_conns[i].wsi;
			wake_count++;
		}
	}
	pthread_mutex_unlock(&g_mutex);
	for (i = 0; i < wake_count; i++) {
		if (wake_list[i])
			lws_callback_on_writable((struct lws *)wake_list[i]);
	}
}

void ws_shutdown_signal(void)
{
	g_ws_shutdown = 1;
	pthread_cond_broadcast(&g_cond);
}

void ws_cleanup(void)
{
	pthread_mutex_lock(&g_mutex);
	ws_msg_t *m = g_queue_head;
	while (m) {
		ws_msg_t *next = m->next;
		free(m->text);
		free(m);
		m = next;
	}
	g_queue_head = NULL;
	g_queue_tail = NULL;
	g_incoming_count = 0;
	m = g_outgoing_head;
	while (m) {
		ws_msg_t *next = m->next;
		free(m->text);
		free(m);
		m = next;
	}
	g_outgoing_head = NULL;
	g_outgoing_tail = NULL;
	g_outgoing_count = 0;
	memset(g_conns, 0, sizeof(g_conns));
	pthread_mutex_unlock(&g_mutex);
}
