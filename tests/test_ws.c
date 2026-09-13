/**
 * @file test_ws.c
 * @brief WebSocket connection table and MSG_MAX enforcement (no libwebsockets).
 */
#define _POSIX_C_SOURCE 200809L

#include "gateway/ws.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#define ASSERT(c)                                                                              \
	do {                                                                                   \
		if (!(c)) {                                                                    \
			fprintf(stderr, "FAIL: %s:%d %s\n", __FILE__, __LINE__, #c);            \
			return 1;                                                                \
		}                                                                                \
	} while (0)
#define RUN(t)                                                                                 \
	do {                                                                                   \
		int _r = (t);                                                                  \
		if (_r)                                                                        \
			return _r;                                                             \
	} while (0)

/** Must match MSG_MAX in src/gateway/ws.c */
#define WS_MSG_MAX 8192

static int test_register_conn_full_table(void)
{
	int i;
	ws_cleanup();
	for (i = 1; i <= 16; i++)
		ASSERT(ws_register_conn(i, (ws_conn_t)(intptr_t)i) == 0);
	ASSERT(ws_register_conn(17, (ws_conn_t)(intptr_t)17) == -1);
	ws_cleanup();
	return 0;
}

static int test_push_incoming_msg_max(void)
{
	char *big;
	char session[32];
	char text[64];
	int got;
	ws_cleanup();
	ASSERT(ws_register_conn(1, (ws_conn_t)(intptr_t)1) == 0);
	big = malloc((size_t)WS_MSG_MAX + 2);
	ASSERT(big != NULL);
	memset(big, 'a', (size_t)WS_MSG_MAX + 1);
	big[WS_MSG_MAX + 1] = '\0';
	ws_push_incoming(1, big);
	got = ws_pop_incoming(session, sizeof(session), text, sizeof(text), 50);
	ASSERT(got == 0);
	ws_push_incoming(1, "ok");
	got = ws_pop_incoming(session, sizeof(session), text, sizeof(text), 500);
	ASSERT(got == 1);
	ASSERT(strcmp(text, "ok") == 0);
	free(big);
	ws_cleanup();
	return 0;
}

static int test_send_to_rejects_oversized(void)
{
	char *big;
	ws_cleanup();
	ASSERT(ws_register_conn(2, (ws_conn_t)(intptr_t)2) == 0);
	big = malloc((size_t)WS_MSG_MAX + 2);
	ASSERT(big != NULL);
	memset(big, 'b', (size_t)WS_MSG_MAX + 1);
	big[WS_MSG_MAX + 1] = '\0';
	ASSERT(ws_send_to("webchat:2", big) != 0);
	ASSERT(ws_send_to("webchat:2", "hi") == 0);
	free(big);
	ws_cleanup();
	return 0;
}

static int test_next_conn_id_and_unregister(void)
{
	int a;
	int b;

	ws_cleanup();
	a = ws_next_conn_id();
	b = ws_next_conn_id();
	ASSERT(b == a + 1);
	ASSERT(ws_register_conn(6, (ws_conn_t)(intptr_t)6) == 0);
	ws_unregister_conn(6);
	ASSERT(ws_send_to("webchat:6", "hi") == 0);
	ws_cleanup();
	return 0;
}

static int test_send_to_rejects_bad_session(void)
{
	ws_cleanup();
	ASSERT(ws_send_to("bad:1", "x") == -1);
	ASSERT(ws_send_to("webchat:99", "x") == 0);
	ws_cleanup();
	return 0;
}

static int test_dequeue_outgoing_and_pending(void)
{
	char buf[64];
	size_t len;
	char session[32];
	char text[64];

	ws_cleanup();
	ASSERT(ws_register_conn(3, (ws_conn_t)(intptr_t)3) == 0);
	ASSERT(ws_send_to("webchat:3", "outmsg") == 0);
	ASSERT(ws_has_pending_outgoing(3) == 1);
	ASSERT(ws_dequeue_outgoing(3, buf, sizeof(buf), &len) == 1);
	ASSERT(len == 6U);
	ASSERT(strcmp(buf, "outmsg") == 0);
	ASSERT(ws_has_pending_outgoing(3) == 0);
	ASSERT(ws_dequeue_outgoing(3, buf, sizeof(buf), &len) == 0);
	ws_push_incoming(3, "in");
	ASSERT(ws_pop_incoming(session, sizeof(session), text, sizeof(text), 500) == 1);
	ASSERT(strcmp(text, "in") == 0);
	ws_cleanup();
	return 0;
}

static int test_broadcast_enqueues_per_conn(void)
{
	char buf[64];
	size_t len;

	ws_cleanup();
	ASSERT(ws_register_conn(4, (ws_conn_t)(intptr_t)4) == 0);
	ASSERT(ws_register_conn(5, (ws_conn_t)(intptr_t)5) == 0);
	ws_broadcast_text("broadcast");
	ASSERT(ws_dequeue_outgoing(4, buf, sizeof(buf), &len) == 1);
	ASSERT(strcmp(buf, "broadcast") == 0);
	ASSERT(ws_dequeue_outgoing(5, buf, sizeof(buf), &len) == 1);
	ASSERT(strcmp(buf, "broadcast") == 0);
	ws_cleanup();
	return 0;
}

static int test_push_incoming_rejects_empty(void)
{
	char session[32];
	char text[64];

	ws_cleanup();
	ASSERT(ws_register_conn(7, (ws_conn_t)(intptr_t)7) == 0);
	ws_push_incoming(7, "");
	ws_push_incoming(7, NULL);
	ASSERT(ws_pop_incoming(session, sizeof(session), text, sizeof(text), 50) == 0);
	ws_cleanup();
	return 0;
}

static int test_pop_incoming_invalid_args(void)
{
	char session[32];
	char text[64];

	ws_cleanup();
	ASSERT(ws_pop_incoming(NULL, sizeof(session), text, sizeof(text), 0) == -1);
	ASSERT(ws_pop_incoming(session, sizeof(session), NULL, sizeof(text), 0) == -1);
	ws_cleanup();
	return 0;
}

static int test_shutdown_stops_pop(void)
{
	char session[32];
	char text[64];

	ws_cleanup();
	ws_shutdown_signal();
	ASSERT(ws_pop_incoming(session, sizeof(session), text, sizeof(text), 50) == 0);
	ws_cleanup();
	return 0;
}

int main(void)
{
	RUN(test_register_conn_full_table());
	RUN(test_push_incoming_msg_max());
	RUN(test_send_to_rejects_oversized());
	RUN(test_next_conn_id_and_unregister());
	RUN(test_send_to_rejects_bad_session());
	RUN(test_dequeue_outgoing_and_pending());
	RUN(test_broadcast_enqueues_per_conn());
	RUN(test_push_incoming_rejects_empty());
	RUN(test_pop_incoming_invalid_args());
	RUN(test_shutdown_stops_pop());
	printf("test_ws: all tests passed\n");
	return 0;
}
