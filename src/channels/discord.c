/**
 * @file discord.c
 * @brief Discord Gateway channel (wss) — standalone libwebsockets client context (not shared with
 *        gateway/http.c) so the HTTP + dashboard WS server and Discord client do not contend on
 *        one event loop. DISPATCH READY stores session/user id; MESSAGE_CREATE enqueue for poll()
 *        (DM or guild mention-only, allowlisted authors). Outbound replies use REST
 *        POST /channels/{id}/messages (libcurl, 429 backoff). See lifecycle comments below.
 *
 * **Gate:** Discord Gateway requires `SHELLCLAW_GATEWAY` (libwebsockets). Builds without gateway ship a
 * stub `channel_discord_get()` whose `init()` fails non-fatally when Discord is enabled (stderr rebuild hint);
 * `main.c` skips registration. `/api/status` still exposes `discord.lifecycle` via `discord_status_snapshot_fill()`.
 *
 * Gateway opcodes (subset): 0 DISPATCH, 1 HEARTBEAT, 6 RESUME, 7 RECONNECT, 9 INVALID SESSION, 10 HELLO,
 * 11 HEARTBEAT ACK. Protocol version 10, JSON encoding (PRD §4.3.15–16).
 *
 * Lifecycle (task 2.5): first HELLO → IDENTIFY; while connected we keep session_id + last seq from
 * DISPATCH. On disconnect / op 7 / failed resume hints we reconnect; after HELLO we send RESUME when
 * session_id and seq exist, else IDENTIFY. op 9 d=false clears session → IDENTIFY on next handshake.
 */
#define _POSIX_C_SOURCE 200809L
#if defined(__APPLE__)
#include <sys/types.h>
#endif

#include "channels/channel.h"
#include "channels/discord_helpers.h"
#include "core/config.h"
#include "cJSON.h"
#include <pthread.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#ifndef SHELLCLAW_GATEWAY

static int discord_stub_init(const config_t *cfg)
{
	if (!cfg || !config_discord_enabled(cfg))
		return 0;
	fprintf(stderr, "shellclaw: discord: rebuild with libwebsockets (gateway) for Discord support\n");
	return -1;
}

static int discord_stub_poll(channel_incoming_msg_t *out, int timeout_ms)
{
	(void)out;
	(void)timeout_ms;
	return 0;
}

static int discord_stub_send(const char *recipient, const char *text,
                             const channel_attachment_t *attachments, size_t att_count)
{
	(void)recipient;
	(void)text;
	(void)attachments;
	(void)att_count;
	return -1;
}

static void discord_stub_cleanup(void) {}

void discord_status_snapshot_fill(const config_t *cfg, discord_status_snapshot_t *out)
{
	if (!out)
		return;
	memset(out, 0, sizeof(*out));
	out->lifecycle = DISCORD_LIFECYCLE_DISABLED;
	if (!cfg || !config_discord_enabled(cfg))
		return;
	snprintf(out->reason, sizeof(out->reason), "%s",
	         "Discord requires SHELLCLAW_GATEWAY (libwebsockets); rebuild with GATEWAY=1");
}

static const channel_t discord_channel = {
	.name = "discord",
	.init = discord_stub_init,
	.poll = discord_stub_poll,
	.send = discord_stub_send,
	.cleanup = discord_stub_cleanup,
};

const channel_t *channel_discord_get(void)
{
	return &discord_channel;
}

void shellclaw_discord_set_live_cfg(const config_t *cfg)
{
	(void)cfg;
}

#else /* SHELLCLAW_GATEWAY */

#include "gateway/lws_compat.h"
#include <libwebsockets.h>
#include <curl/curl.h>
#include <strings.h>
#include <stdatomic.h>

/** PRD §4.3.19: GUILDS | GUILD_MESSAGES | MESSAGE_CONTENT | DIRECT_MESSAGES */
#define DISCORD_INTENTS 37377u

#define GATEWAY_HOST "gateway.discord.gg"
#define GATEWAY_PATH "/?v=10&encoding=json"
#define GATEWAY_PORT 443
#define RX_MAX (512 * 1024)
#define WRITE_CHUNK 65536
#define DISCORD_API_MSG_URL "https://discord.com/api/v10/channels/%s/messages"
#define DISCORD_SEND_MAX_ATTEMPTS 6
#define DISCORD_SEND_BASE_BACKOFF_MS 400
#define DISCORD_MSG_MAX_LEN 2000
#define DISCORD_RECIPIENT_PREFIX "discord:c:"
#define DISCORD_CHANNEL_ID_CAP 32
#define DISCORD_AUTH_HDR_CAP 384
#define DISCORD_URL_CAP 512
#define DISCORD_SEND_TIMEOUT_SEC 30L
#define DISCORD_SEND_CONNECT_SEC 10L
#define DISCORD_SEND_WAIT_CAP_MS 300000
#define DISCORD_RECONNECT_BASE_MS 750
#define DISCORD_RECONNECT_MAX_MS 120000

struct discord_userdata {
	char placeholder;
};

struct discord_pending {
	channel_incoming_msg_t msg;
	struct discord_pending *next;
};

struct discord_ctx {
	pthread_mutex_t lock;
	pthread_cond_t queue_cond;
	const config_t *cfg;
	char *token;
	struct lws_context *lws_ctx;
	struct lws *wsi;
	pthread_t thread;
	volatile int run_service;
	char *rx_buf;
	size_t rx_len;
	size_t rx_cap;
	int heartbeat_interval_ms;
	uint64_t last_seq;
	char *session_id;
	char *bot_user_id;
	int saw_hello;
	volatile int pending_identify;
	volatile int pending_resume;
	volatile int pending_periodic_hb;
	volatile int gateway_ready;
	int thread_created;
	volatile int schedule_reconnect;
	volatile int outbound_connect;
	int reconnect_backoff_ms;
	uint64_t reconnect_deadline_ms;
	struct discord_pending *q_head;
	struct discord_pending *q_tail;
};

static struct discord_ctx g_dc;
static int discord_jitter_ms(void);
static _Atomic int g_discord_runtime_active;

static uint64_t discord_now_ms(void)
{
	struct timespec ts;
	if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
		return 0;
	return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}

static void discord_rx_reset(struct discord_ctx *dc)
{
	dc->rx_len = 0;
}

/** Appends one RX fragment; returns 0 ok, -1 overflow. */
static int discord_rx_append(struct discord_ctx *dc, const void *in, size_t len)
{
	if (len > RX_MAX || dc->rx_len > RX_MAX - len)
		return -1;
	if (dc->rx_cap < dc->rx_len + len) {
		size_t need = dc->rx_len + len + 1;
		size_t ncap = dc->rx_cap ? dc->rx_cap * 2 : 4096;
		while (ncap < need && ncap < RX_MAX)
			ncap *= 2;
		if (need > RX_MAX)
			return -1;
		char *p = realloc(dc->rx_buf, ncap);
		if (!p)
			return -1;
		dc->rx_buf = p;
		dc->rx_cap = ncap;
	}
	memcpy(dc->rx_buf + dc->rx_len, in, len);
	dc->rx_len += len;
	dc->rx_buf[dc->rx_len] = '\0';
	return 0;
}

static int discord_send_json(struct lws *wsi, const char *json)
{
	size_t n = strlen(json);
	if (n == 0 || n > WRITE_CHUNK)
		return -1;
	unsigned char *buf = malloc(LWS_PRE + n);
	if (!buf)
		return -1;
	memcpy(buf + LWS_PRE, json, n);
	int w = (int)lws_write(wsi, buf + LWS_PRE, n, LWS_WRITE_TEXT);
	free(buf);
	if (w < (int)n) {
		fprintf(stderr, "shellclaw: discord: short write on gateway\n");
		return -1;
	}
	return 0;
}

static char *discord_build_identify(const char *token)
{
	cJSON *root = cJSON_CreateObject();
	cJSON *d = cJSON_CreateObject();
	cJSON *props = cJSON_CreateObject();
	char *out = NULL;
	if (!root || !d || !props)
		goto fail;
	cJSON_AddNumberToObject(root, "op", 2);
	cJSON_AddItemToObject(root, "d", d);
	cJSON_AddStringToObject(d, "token", token);
	cJSON_AddItemToObject(d, "properties", props);
	cJSON_AddStringToObject(props, "os", "linux");
	cJSON_AddStringToObject(props, "browser", "shellclaw");
	cJSON_AddStringToObject(props, "device", "shellclaw");
	cJSON_AddNumberToObject(d, "intents", (double)DISCORD_INTENTS);
	props = NULL;
	d = NULL;
	out = cJSON_PrintUnformatted(root);
fail:
	cJSON_Delete(props);
	cJSON_Delete(d);
	cJSON_Delete(root);
	return out;
}

static char *discord_build_resume(const char *token, const char *session_id, uint64_t seq)
{
	cJSON *root = cJSON_CreateObject();
	cJSON *d = cJSON_CreateObject();
	char *out = NULL;
	if (!root || !d)
		goto fail;
	if (!token || !session_id || session_id[0] == '\0' || seq == 0)
		goto fail;
	cJSON_AddNumberToObject(root, "op", 6);
	cJSON_AddStringToObject(d, "token", token);
	cJSON_AddStringToObject(d, "session_id", session_id);
	cJSON_AddNumberToObject(d, "seq", (double)seq);
	cJSON_AddItemToObject(root, "d", d);
	d = NULL;
	out = cJSON_PrintUnformatted(root);
fail:
	cJSON_Delete(d);
	cJSON_Delete(root);
	return out;
}

static char *discord_build_heartbeat_null(void)
{
	cJSON *root = cJSON_CreateObject();
	char *out = NULL;
	if (!root)
		return NULL;
	cJSON_AddNumberToObject(root, "op", 1);
	cJSON_AddNullToObject(root, "d");
	out = cJSON_PrintUnformatted(root);
	cJSON_Delete(root);
	return out;
}

static char *discord_build_heartbeat_seq(uint64_t seq)
{
	cJSON *root = cJSON_CreateObject();
	char *out = NULL;
	if (!root)
		return NULL;
	cJSON_AddNumberToObject(root, "op", 1);
	cJSON_AddNumberToObject(root, "d", (double)seq);
	out = cJSON_PrintUnformatted(root);
	cJSON_Delete(root);
	return out;
}

static int discord_is_user_allowed(const config_t *cfg, const char *author_id)
{
	int n = config_discord_allowed_user_ids_count(cfg);
	int i;
	for (i = 0; i < n; i++) {
		const char *allow = config_discord_allowed_user_id(cfg, i);
		if (discord_helpers_allow_entry_equals(allow, author_id))
			return 1;
	}
	return 0;
}

static void discord_queue_enqueue(struct discord_ctx *dc, channel_incoming_msg_t *msg)
{
	struct discord_pending *n = calloc(1, sizeof(*n));
	if (!n) {
		channel_incoming_msg_clear(msg);
		return;
	}
	n->msg = *msg;
	memset(msg, 0, sizeof(*msg));
	pthread_mutex_lock(&dc->lock);
	if (dc->q_tail)
		dc->q_tail->next = n;
	else
		dc->q_head = n;
	dc->q_tail = n;
	pthread_cond_signal(&dc->queue_cond);
	pthread_mutex_unlock(&dc->lock);
}

static void discord_abs_timeout_ms(int timeout_ms, struct timespec *out)
{
	clock_gettime(CLOCK_REALTIME, out);
	out->tv_sec += timeout_ms / 1000;
	out->tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
	while (out->tv_nsec >= 1000000000L) {
		out->tv_nsec -= 1000000000L;
		out->tv_sec++;
	}
}

/** PRD §4.3 · task 2.3: bots ignored; allowlist by author.id; guilds mention-only (@bot); session_id discord:c:<channel_id>. */
static void discord_on_message_create(struct discord_ctx *dc, cJSON *d)
{
	cJSON *author;
	cJSON *bot_flag;
	cJSON *aid_item;
	char *author_id = NULL;
	cJSON *guild_id;
	int is_guild;
	char *bid = NULL;
	cJSON *ch;
	cJSON *content;
	const char *txt;
	char sess_buf[128];
	channel_incoming_msg_t m = { 0 };
	if (!dc || !dc->cfg || !d || !cJSON_IsObject(d))
		return;
	author = cJSON_GetObjectItem(d, "author");
	if (!cJSON_IsObject(author))
		return;
	bot_flag = cJSON_GetObjectItem(author, "bot");
	if (cJSON_IsTrue(bot_flag))
		return;
	aid_item = cJSON_GetObjectItem(author, "id");
	if (cJSON_IsString(aid_item) && aid_item->valuestring)
		author_id = strdup(aid_item->valuestring);
	if (!author_id)
		return;
	if (!discord_is_user_allowed(dc->cfg, author_id)) {
		free(author_id);
		return;
	}
	guild_id = cJSON_GetObjectItem(d, "guild_id");
	is_guild = cJSON_IsString(guild_id) && guild_id->valuestring && guild_id->valuestring[0] != '\0';
	if (is_guild) {
		cJSON *mentions;
		int mention_ok;
		pthread_mutex_lock(&dc->lock);
		bid = dc->bot_user_id ? strdup(dc->bot_user_id) : NULL;
		pthread_mutex_unlock(&dc->lock);
		mentions = cJSON_GetObjectItem(d, "mentions");
		mention_ok = bid != NULL &&
		             discord_helpers_mentions_include_bot(mentions, bid);
		free(bid);
		if (!mention_ok) {
			free(author_id);
			return;
		}
	}
	ch = cJSON_GetObjectItem(d, "channel_id");
	if (!cJSON_IsString(ch) || !ch->valuestring || ch->valuestring[0] == '\0') {
		free(author_id);
		return;
	}
	content = cJSON_GetObjectItem(d, "content");
	txt = "";
	if (cJSON_IsString(content) && content->valuestring)
		txt = content->valuestring;
	if (!txt[0]) {
		free(author_id);
		return;
	}
	if (discord_helpers_session_id_from_channel(ch->valuestring, sess_buf, sizeof(sess_buf)) != 0) {
		free(author_id);
		return;
	}
	m.session_id = strdup(sess_buf);
	if (!m.session_id) {
		free(author_id);
		return;
	}
	m.user_id = author_id;
	m.text = strdup(txt);
	if (!m.text) {
		channel_incoming_msg_clear(&m);
		return;
	}
	discord_queue_enqueue(dc, &m);
}

static int discord_has_resume_state(struct discord_ctx *dc)
{
	int ok;
	pthread_mutex_lock(&dc->lock);
	ok = dc->session_id != NULL && dc->session_id[0] != '\0' && dc->last_seq > 0;
	pthread_mutex_unlock(&dc->lock);
	return ok;
}

static void discord_handle_dispatch(struct discord_ctx *dc, cJSON *root)
{
	cJSON *t = cJSON_GetObjectItem(root, "t");
	cJSON *d = cJSON_GetObjectItem(root, "d");
	cJSON *s = cJSON_GetObjectItem(root, "s");
	if (cJSON_IsNumber(s)) {
		pthread_mutex_lock(&dc->lock);
		dc->last_seq = (uint64_t)s->valuedouble;
		pthread_mutex_unlock(&dc->lock);
	}
	if (!cJSON_IsString(t) || !t->valuestring || !d)
		return;
	if (strcmp(t->valuestring, "READY") == 0) {
		cJSON *sess = cJSON_GetObjectItem(d, "session_id");
		cJSON *user = cJSON_GetObjectItem(d, "user");
		cJSON *uid = user ? cJSON_GetObjectItem(user, "id") : NULL;
		pthread_mutex_lock(&dc->lock);
		free(dc->session_id);
		dc->session_id = cJSON_IsString(sess) && sess->valuestring ? strdup(sess->valuestring) : NULL;
		free(dc->bot_user_id);
		dc->bot_user_id = cJSON_IsString(uid) && uid->valuestring ? strdup(uid->valuestring) : NULL;
		dc->gateway_ready = 1;
		pthread_mutex_unlock(&dc->lock);
		fprintf(stderr, "shellclaw: discord: gateway READY\n");
	} else if (strcmp(t->valuestring, "RESUMED") == 0) {
		dc->gateway_ready = 1;
		fprintf(stderr, "shellclaw: discord: gateway RESUMED\n");
	} else if (strcmp(t->valuestring, "MESSAGE_CREATE") == 0) {
		discord_on_message_create(dc, d);
	}
}

static void discord_handle_payload(struct discord_ctx *dc, struct lws *wsi, const char *json)
{
	cJSON *root = cJSON_Parse(json);
	cJSON *op;
	cJSON *d;
	if (!root)
		return;
	op = cJSON_GetObjectItem(root, "op");
	if (!cJSON_IsNumber(op)) {
		cJSON_Delete(root);
		return;
	}
	int opcode = (int)op->valuedouble;
	d = cJSON_GetObjectItem(root, "d");
	switch (opcode) {
	case 7:
		fprintf(stderr, "shellclaw: discord: gateway RECONNECT (opcode 7)\n");
		dc->schedule_reconnect = 1;
		dc->reconnect_deadline_ms = 0;
		if (dc->wsi)
			lws_close_reason(dc->wsi, LWS_CLOSE_STATUS_NORMAL, NULL, 0);
		break;
	case 9:
		if (!cJSON_IsBool(d))
			break;
		if (cJSON_IsFalse(d)) {
			pthread_mutex_lock(&dc->lock);
			free(dc->session_id);
			dc->session_id = NULL;
			dc->last_seq = 0;
			pthread_mutex_unlock(&dc->lock);
			fprintf(stderr,
			        "shellclaw: discord: INVALID_SESSION — new IDENTIFY required\n");
		} else {
			fprintf(stderr, "shellclaw: discord: INVALID_SESSION — retry Resume\n");
		}
		dc->schedule_reconnect = 1;
		dc->reconnect_deadline_ms = 0;
		if (dc->wsi)
			lws_close_reason(dc->wsi, LWS_CLOSE_STATUS_NORMAL, NULL, 0);
		break;
	case 10: {
		cJSON *ival;
		if (!cJSON_IsObject(d)) {
			cJSON_Delete(root);
			return;
		}
		ival = cJSON_GetObjectItem(d, "heartbeat_interval");
		if (cJSON_IsNumber(ival))
			dc->heartbeat_interval_ms = (int)ival->valuedouble;
		dc->saw_hello = 1;
		dc->pending_identify = 0;
		dc->pending_resume = 0;
		if (discord_has_resume_state(dc))
			dc->pending_resume = 1;
		else
			dc->pending_identify = 1;
		lws_callback_on_writable(wsi);
		break;
	}
	case 11:
		break;
	case 0:
		discord_handle_dispatch(dc, root);
		break;
	default:
		break;
	}
	cJSON_Delete(root);
}

static int callback_discord(struct lws *wsi, enum lws_callback_reasons reason,
                            void *user, void *in, size_t len)
{
	struct discord_ctx *dc = &g_dc;
	(void)user;
	switch (reason) {
	case LWS_CALLBACK_CLIENT_ESTABLISHED:
		dc->wsi = wsi;
		dc->outbound_connect = 0;
		dc->schedule_reconnect = 0;
		dc->reconnect_backoff_ms = DISCORD_RECONNECT_BASE_MS;
		dc->reconnect_deadline_ms = 0;
		discord_rx_reset(dc);
		break;
	case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
		fprintf(stderr, "shellclaw: discord: connection error (%s)\n",
		        in ? (const char *)in : "?");
		dc->wsi = NULL;
		dc->outbound_connect = 0;
		dc->saw_hello = 0;
		dc->pending_identify = 0;
		dc->pending_resume = 0;
		dc->pending_periodic_hb = 0;
		dc->heartbeat_interval_ms = 0;
		dc->gateway_ready = 0;
		if (dc->run_service) {
			dc->schedule_reconnect = 1;
			dc->reconnect_deadline_ms = 0;
		}
		break;
	case LWS_CALLBACK_CLIENT_RECEIVE:
		if (discord_rx_append(dc, in, len) != 0) {
			fprintf(stderr, "shellclaw: discord: gateway payload too large\n");
			discord_rx_reset(dc);
			break;
		}
		if (lws_is_final_fragment(wsi)) {
			discord_handle_payload(dc, wsi, dc->rx_buf);
			discord_rx_reset(dc);
		}
		break;
	case LWS_CALLBACK_CLIENT_WRITEABLE:
		if (dc->pending_resume && dc->token && dc->saw_hello) {
			char *resume_json;
			char *sid = NULL;
			uint64_t seq;
			dc->pending_resume = 0;
			pthread_mutex_lock(&dc->lock);
			if (dc->session_id)
				sid = strdup(dc->session_id);
			seq = dc->last_seq;
			pthread_mutex_unlock(&dc->lock);
			if (sid && seq > 0 && (resume_json = discord_build_resume(dc->token, sid, seq))) {
				free(sid);
				(void)discord_send_json(wsi, resume_json);
				free(resume_json);
			} else {
				free(sid);
				dc->pending_identify = 1;
				lws_callback_on_writable(wsi);
			}
		} else if (dc->pending_identify && dc->token && dc->saw_hello) {
			char *id_json = discord_build_identify(dc->token);
			dc->pending_identify = 0;
			if (id_json) {
				(void)discord_send_json(wsi, id_json);
				free(id_json);
			}
		}
		if (dc->pending_periodic_hb) {
			char *hb;
			uint64_t seq;
			dc->pending_periodic_hb = 0;
			pthread_mutex_lock(&dc->lock);
			seq = dc->last_seq;
			pthread_mutex_unlock(&dc->lock);
			if (seq > 0)
				hb = discord_build_heartbeat_seq(seq);
			else
				hb = discord_build_heartbeat_null();
			if (hb) {
				(void)discord_send_json(wsi, hb);
				free(hb);
			}
		}
		break;
	case LWS_CALLBACK_CLIENT_CLOSED:
		dc->wsi = NULL;
		dc->outbound_connect = 0;
		dc->saw_hello = 0;
		dc->pending_identify = 0;
		dc->pending_resume = 0;
		dc->pending_periodic_hb = 0;
		dc->heartbeat_interval_ms = 0;
		dc->gateway_ready = 0;
		if (dc->run_service) {
			dc->schedule_reconnect = 1;
			dc->reconnect_deadline_ms = 0;
		}
		break;
	default:
		break;
	}
	return 0;
}

static struct lws_protocols discord_protocols[] = {
	{
		.name = "discord-gateway",
		.callback = callback_discord,
		.per_session_data_size = sizeof(struct discord_userdata),
		.rx_buffer_size = 65536,
	},
	{ .name = NULL },
};

static int discord_start_client_connect(struct discord_ctx *dc)
{
	struct lws_client_connect_info cc;
	if (!dc->lws_ctx)
		return -1;
	memset(&cc, 0, sizeof(cc));
	cc.context = dc->lws_ctx;
	cc.address = GATEWAY_HOST;
	cc.port = GATEWAY_PORT;
	cc.path = GATEWAY_PATH;
	cc.host = cc.address;
	cc.origin = "https://discord.com";
	cc.protocol = discord_protocols[0].name;
	cc.ssl_connection = LCCSCF_USE_SSL;
	cc.pwsi = &dc->wsi;
	if (lws_client_connect_via_info(&cc) == NULL) {
		fprintf(stderr, "shellclaw: discord: gateway connect failed to start\n");
		return -1;
	}
	return 0;
}

static void *discord_thread_main(void *arg)
{
	struct discord_ctx *dc = (struct discord_ctx *)arg;
	uint64_t next_hb_ms = 0;
	while (dc->run_service) {
		if (dc->lws_ctx)
			(void)lws_service(dc->lws_ctx, 50);
		if (!dc->run_service)
			break;
		if (dc->schedule_reconnect && dc->lws_ctx && !dc->wsi && !dc->outbound_connect &&
		    dc->run_service) {
			uint64_t now = discord_now_ms();
			if (dc->reconnect_deadline_ms == 0)
				dc->reconnect_deadline_ms =
				    now + (uint64_t)dc->reconnect_backoff_ms +
				    (uint64_t)(discord_jitter_ms() % 400);
			if (now >= dc->reconnect_deadline_ms) {
				if (discord_start_client_connect(dc) != 0) {
					dc->reconnect_deadline_ms =
					    now + (uint64_t)dc->reconnect_backoff_ms +
					    (uint64_t)(discord_jitter_ms() % 400);
					if (dc->reconnect_backoff_ms < DISCORD_RECONNECT_MAX_MS) {
						int nb = dc->reconnect_backoff_ms * 2;
						dc->reconnect_backoff_ms =
						    nb > DISCORD_RECONNECT_MAX_MS ? DISCORD_RECONNECT_MAX_MS : nb;
					}
				} else {
					dc->reconnect_deadline_ms = 0;
					dc->outbound_connect = 1;
				}
			}
		} else if (dc->wsi)
			dc->reconnect_deadline_ms = 0;
		if (!dc->run_service)
			break;
		if (dc->wsi && dc->saw_hello && dc->heartbeat_interval_ms > 0) {
			uint64_t now = discord_now_ms();
			if (next_hb_ms == 0)
				next_hb_ms = now + (uint64_t)dc->heartbeat_interval_ms;
			if (now >= next_hb_ms) {
				dc->pending_periodic_hb = 1;
				lws_callback_on_writable(dc->wsi);
				next_hb_ms = now + (uint64_t)dc->heartbeat_interval_ms;
			}
		} else if (!dc->wsi)
			next_hb_ms = 0;
	}
	return NULL;
}

static int discord_init(const config_t *cfg)
{
	struct lws_context_creation_info info;
	const char *envname;
	const char *tok;
	if (!cfg || !config_discord_enabled(cfg))
		return 0;
	envname = config_discord_token_env(cfg);
	tok = envname ? getenv(envname) : NULL;
	if (!tok || !tok[0]) {
		fprintf(stderr, "shellclaw: discord: missing bot token (env %s)\n",
		        envname ? envname : "(null)");
		return -1;
	}
	memset(&g_dc, 0, sizeof(g_dc));
	if (pthread_mutex_init(&g_dc.lock, NULL) != 0)
		return -1;
	if (pthread_cond_init(&g_dc.queue_cond, NULL) != 0) {
		pthread_mutex_destroy(&g_dc.lock);
		return -1;
	}
	g_dc.cfg = cfg;
	g_dc.token = strdup(tok);
	if (!g_dc.token) {
		pthread_cond_destroy(&g_dc.queue_cond);
		pthread_mutex_destroy(&g_dc.lock);
		return -1;
	}
	memset(&info, 0, sizeof(info));
	info.port = CONTEXT_PORT_NO_LISTEN;
	info.protocols = discord_protocols;
	info.options = LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;
	info.gid = -1;
	info.uid = -1;
	g_dc.lws_ctx = lws_create_context(&info);
	if (!g_dc.lws_ctx) {
		free(g_dc.token);
		g_dc.token = NULL;
		pthread_cond_destroy(&g_dc.queue_cond);
		pthread_mutex_destroy(&g_dc.lock);
		return -1;
	}
	g_dc.reconnect_backoff_ms = DISCORD_RECONNECT_BASE_MS;
	if (discord_start_client_connect(&g_dc) != 0) {
		lws_context_destroy(g_dc.lws_ctx);
		g_dc.lws_ctx = NULL;
		free(g_dc.token);
		g_dc.token = NULL;
		pthread_cond_destroy(&g_dc.queue_cond);
		pthread_mutex_destroy(&g_dc.lock);
		return -1;
	}
	g_dc.run_service = 1;
	if (pthread_create(&g_dc.thread, NULL, discord_thread_main, &g_dc) != 0) {
		g_dc.run_service = 0;
		lws_context_destroy(g_dc.lws_ctx);
		g_dc.lws_ctx = NULL;
		free(g_dc.token);
		g_dc.token = NULL;
		pthread_cond_destroy(&g_dc.queue_cond);
		pthread_mutex_destroy(&g_dc.lock);
		return -1;
	}
	g_dc.thread_created = 1;
	atomic_store_explicit(&g_discord_runtime_active, 1, memory_order_release);
	return 0;
}

static int discord_poll(channel_incoming_msg_t *out, int timeout_ms)
{
	struct timespec abstime;
	struct discord_ctx *dc = &g_dc;
	struct discord_pending *node;
	if (!out || !dc->cfg)
		return -1;
	memset(out, 0, sizeof(*out));
	if (timeout_ms < 0)
		timeout_ms = 0;
	discord_abs_timeout_ms(timeout_ms, &abstime);
	pthread_mutex_lock(&dc->lock);
	for (;;) {
		if (dc->q_head) {
			node = dc->q_head;
			dc->q_head = node->next;
			if (!dc->q_head)
				dc->q_tail = NULL;
			*out = node->msg;
			free(node);
			pthread_mutex_unlock(&dc->lock);
			return 1;
		}
		if (!dc->run_service) {
			pthread_mutex_unlock(&dc->lock);
			return 0;
		}
		{
			int wait_rc = pthread_cond_timedwait(&dc->queue_cond, &dc->lock, &abstime);
			if (wait_rc == ETIMEDOUT) {
				pthread_mutex_unlock(&dc->lock);
				return 0;
			}
		}
	}
}

static void discord_sleep_ms(int ms)
{
	struct timespec ts;
	if (ms <= 0)
		return;
	ts.tv_sec = (time_t)(ms / 1000);
	ts.tv_nsec = (long)(ms % 1000) * 1000000L;
	while (nanosleep(&ts, &ts) == -1 && errno == EINTR)
		;
}

static int discord_jitter_ms(void)
{
	struct timespec ts;
	if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
		return 0;
	return (int)(ts.tv_nsec % 251);
}

static size_t discord_discard_write_cb(char *ptr, size_t size, size_t nmemb, void *userdata)
{
	(void)ptr;
	(void)userdata;
	return size * nmemb;
}

struct discord_hdr_ud {
	double retry_after_sec;
};

static size_t discord_header_retry_after_cb(char *buffer, size_t size, size_t nitems,
                                            void *userdata)
{
	size_t total;
	struct discord_hdr_ud *h;
	char line[96];
	size_t i;
	size_t copy;
	total = size * nitems;
	h = (struct discord_hdr_ud *)userdata;
	if (!h || total < 13)
		return total;
	copy = total < sizeof(line) - 1 ? total : sizeof(line) - 1;
	memcpy(line, buffer, copy);
	line[copy] = '\0';
	for (i = 0; i < copy && line[i] != '\r' && line[i] != '\n'; i++)
		;
	line[i] = '\0';
	if (strncasecmp(line, "retry-after:", 12) != 0)
		return total;
	h->retry_after_sec = strtod(line + 12, NULL);
	return total;
}

static int discord_recipient_channel_id(const char *recipient, char *out, size_t outsiz)
{
	size_t plen;
	size_t idlen;
	if (!recipient || !out || outsiz == 0)
		return -1;
	plen = strlen(DISCORD_RECIPIENT_PREFIX);
	if (strncmp(recipient, DISCORD_RECIPIENT_PREFIX, plen) != 0)
		return -1;
	recipient += plen;
	idlen = strlen(recipient);
	if (idlen == 0 || idlen >= outsiz)
		return -1;
	memcpy(out, recipient, idlen + 1);
	return 0;
}

static int discord_send(const char *recipient, const char *text,
                        const channel_attachment_t *attachments, size_t att_count)
{
	struct discord_ctx *dc = &g_dc;
	char channel_id[DISCORD_CHANNEL_ID_CAP];
	cJSON *root;
	char *json_body;
	char url[DISCORD_URL_CAP];
	char auth_hdr[DISCORD_AUTH_HDR_CAP];
	int attempt;
	(void)attachments;
	if (att_count > 0)
		return -1;
	if (!dc->token || !recipient || !text || text[0] == '\0')
		return -1;
	if (strlen(text) > (size_t)DISCORD_MSG_MAX_LEN)
		return -1;
	if (discord_recipient_channel_id(recipient, channel_id, sizeof(channel_id)) != 0)
		return -1;
	root = cJSON_CreateObject();
	if (!root)
		return -1;
	cJSON_AddStringToObject(root, "content", text);
	json_body = cJSON_PrintUnformatted(root);
	cJSON_Delete(root);
	if (!json_body)
		return -1;
	if (snprintf(url, sizeof(url), DISCORD_API_MSG_URL, channel_id) >= (int)sizeof(url)) {
		free(json_body);
		return -1;
	}
	if (snprintf(auth_hdr, sizeof(auth_hdr), "Authorization: Bot %s", dc->token) >= (int)sizeof(auth_hdr)) {
		free(json_body);
		return -1;
	}
	for (attempt = 0; attempt < DISCORD_SEND_MAX_ATTEMPTS; attempt++) {
		struct discord_hdr_ud hd = { -1.0 };
		struct curl_slist *hdrs = NULL;
		CURL *curl;
		CURLcode cr;
		long http_code = 0;
		curl = curl_easy_init();
		if (!curl) {
			free(json_body);
			return -1;
		}
		hdrs = curl_slist_append(hdrs, auth_hdr);
		hdrs = curl_slist_append(hdrs, "Content-Type: application/json");
		if (!hdrs) {
			curl_easy_cleanup(curl);
			free(json_body);
			return -1;
		}
		curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
		curl_easy_setopt(curl, CURLOPT_URL, url);
		curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_body);
		curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)strlen(json_body));
		curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
		curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, DISCORD_SEND_CONNECT_SEC);
		curl_easy_setopt(curl, CURLOPT_TIMEOUT, DISCORD_SEND_TIMEOUT_SEC);
		curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, discord_header_retry_after_cb);
		curl_easy_setopt(curl, CURLOPT_HEADERDATA, &hd);
		curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, discord_discard_write_cb);
		cr = curl_easy_perform(curl);
		curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
		curl_slist_free_all(hdrs);
		curl_easy_cleanup(curl);
		if (cr != CURLE_OK) {
			fprintf(stderr, "shellclaw: discord: send curl error: %s\n", curl_easy_strerror(cr));
			free(json_body);
			return -1;
		}
		if (http_code >= 200 && http_code < 300) {
			free(json_body);
			return 0;
		}
		if (http_code == 429 && attempt + 1 < DISCORD_SEND_MAX_ATTEMPTS) {
			int sleep_ms = discord_helpers_send_backoff_ms(
			    attempt, hd.retry_after_sec, discord_jitter_ms(),
			    DISCORD_SEND_BASE_BACKOFF_MS, DISCORD_SEND_WAIT_CAP_MS);
			discord_sleep_ms(sleep_ms);
			continue;
		}
		fprintf(stderr, "shellclaw: discord: send failed HTTP %ld\n", http_code);
		free(json_body);
		return -1;
	}
	return -1;
}

static void discord_cleanup(void)
{
	struct discord_pending *pending;
	struct discord_pending *next;
	atomic_store_explicit(&g_discord_runtime_active, 0, memory_order_release);
	g_dc.run_service = 0;
	if (g_dc.lws_ctx)
		lws_cancel_service(g_dc.lws_ctx);
	pthread_mutex_lock(&g_dc.lock);
	pthread_cond_broadcast(&g_dc.queue_cond);
	for (pending = g_dc.q_head; pending; pending = next) {
		next = pending->next;
		channel_incoming_msg_clear(&pending->msg);
		free(pending);
	}
	g_dc.q_head = NULL;
	g_dc.q_tail = NULL;
	pthread_mutex_unlock(&g_dc.lock);
	if (g_dc.thread_created) {
		pthread_join(g_dc.thread, NULL);
		g_dc.thread_created = 0;
	}
	if (g_dc.lws_ctx) {
		lws_context_destroy(g_dc.lws_ctx);
		g_dc.lws_ctx = NULL;
	}
	free(g_dc.token);
	g_dc.token = NULL;
	free(g_dc.rx_buf);
	g_dc.rx_buf = NULL;
	g_dc.rx_cap = 0;
	g_dc.rx_len = 0;
	pthread_mutex_lock(&g_dc.lock);
	free(g_dc.session_id);
	g_dc.session_id = NULL;
	free(g_dc.bot_user_id);
	g_dc.bot_user_id = NULL;
	pthread_mutex_unlock(&g_dc.lock);
	pthread_cond_destroy(&g_dc.queue_cond);
	pthread_mutex_destroy(&g_dc.lock);
}

void shellclaw_discord_set_live_cfg(const config_t *cfg)
{
	if (!cfg)
		return;
	if (g_dc.token == NULL) {
		g_dc.cfg = cfg;
		return;
	}
	pthread_mutex_lock(&g_dc.lock);
	g_dc.cfg = cfg;
	pthread_mutex_unlock(&g_dc.lock);
}

void discord_status_snapshot_fill(const config_t *cfg, discord_status_snapshot_t *out)
{
	struct discord_ctx *dc = &g_dc;
	struct lws *wsi_snap;
	int gw_ready;
	int sched;
	int outbound;
	if (!out)
		return;
	memset(out, 0, sizeof(*out));
	out->lifecycle = DISCORD_LIFECYCLE_DISABLED;
	if (!cfg || !config_discord_enabled(cfg))
		return;
	if (!atomic_load_explicit(&g_discord_runtime_active, memory_order_acquire)) {
		out->lifecycle = DISCORD_LIFECYCLE_DISCONNECTED;
		snprintf(out->reason, sizeof(out->reason), "%s", "not running");
		return;
	}
	wsi_snap = dc->wsi;
	pthread_mutex_lock(&dc->lock);
	gw_ready = dc->gateway_ready;
	sched = dc->schedule_reconnect;
	outbound = dc->outbound_connect;
	pthread_mutex_unlock(&dc->lock);
	if (gw_ready) {
		out->lifecycle = DISCORD_LIFECYCLE_CONNECTED;
		return;
	}
	if (sched) {
		out->lifecycle = DISCORD_LIFECYCLE_RECONNECTING;
		return;
	}
	if (outbound || wsi_snap != NULL) {
		out->lifecycle = DISCORD_LIFECYCLE_CONNECTING;
		return;
	}
	out->lifecycle = DISCORD_LIFECYCLE_CONNECTING;
}

static const channel_t discord_channel = {
	.name = "discord",
	.init = discord_init,
	.poll = discord_poll,
	.send = discord_send,
	.cleanup = discord_cleanup,
};

const channel_t *channel_discord_get(void)
{
	return &discord_channel;
}

#endif /* SHELLCLAW_GATEWAY */
