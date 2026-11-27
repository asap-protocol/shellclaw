/**
 * @file webchat.c
 * @brief Process WebChat turns and messages from native WebSocket clients.
 */
#define _POSIX_C_SOURCE 200809L

#include "channels/channel.h"
#include "channels/webchat.h"
#include "gateway/ws.h"
#include "core/config.h"
#include "core/memory.h"
#include "crypto/crypto.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WEBCHAT_USER_ID "webchat"
#define NATIVE_SESSION_PREFIX "webchat:socket:"
#define WS_SESSION_PREFIX "webchat:"

static char g_native_prefix[sizeof(NATIVE_SESSION_PREFIX) + 17];

static int webchat_init(const config_t *cfg)
{
	(void)cfg;
	uint64_t id;
	if (crypto_read_urandom(&id, sizeof(id)) != 0) return -1;
	snprintf(g_native_prefix, sizeof(g_native_prefix), NATIVE_SESSION_PREFIX "%016" PRIx64 ":", id);
	return 0;
}

static int webchat_poll(channel_incoming_msg_t *out, int timeout_ms)
{
	if (!out) return -1;
	memset(out, 0, sizeof(*out));
	char session_id[128];
	char text[WS_TEXT_BUF_SIZE];
	int r = webchat_store_claim(session_id, sizeof(session_id), text, sizeof(text));
	int from_device = r == 1;
	if (!from_device) {
		if (r < 0) fprintf(stderr, "Cannot read the WebChat queue.\n");
		char socket_session[32];
		r = ws_pop_incoming(socket_session, sizeof(socket_session), text, sizeof(text), timeout_ms);
		if (r != 1) return r;
		/* Connection numbers can repeat after restart. Give each run its own prefix. */
		snprintf(session_id, sizeof(session_id), "%s%s", g_native_prefix,
		         socket_session + sizeof(WS_SESSION_PREFIX) - 1);
	}
	out->session_id = strdup(session_id);
	out->user_id = strdup(WEBCHAT_USER_ID);
	out->text = strdup(text);
	out->attachments = NULL;
	out->attachments_count = 0;
	if (!out->session_id || !out->user_id || !out->text) {
		if (from_device)
			webchat_store_finish(session_id, "Cannot process the message.");
		channel_incoming_msg_clear(out);
		return -1;
	}
	return 1;
}

static int webchat_send(const char *recipient, const char *text,
                        const channel_attachment_t *attachments, size_t att_count)
{
	(void)attachments;
	(void)att_count;
	if (!recipient || !text) return -1;
	if (strncmp(recipient, WEBCHAT_DEVICE_PREFIX, sizeof(WEBCHAT_DEVICE_PREFIX) - 1) == 0)
		return webchat_store_finish(recipient, text);
	size_t prefix_size = strlen(g_native_prefix);
	if (!prefix_size || strncmp(recipient, g_native_prefix, prefix_size) != 0)
		return -1;
	char socket_session[32];
	int n = snprintf(socket_session, sizeof(socket_session), WS_SESSION_PREFIX "%s",
	                 recipient + prefix_size);
	if (n < 0 || (size_t)n >= sizeof(socket_session)) return -1;
	return ws_send_to(socket_session, text);
}

static void webchat_cleanup(void)
{
}

static const channel_t webchat_channel = {
	.name = "webchat",
	.init = webchat_init,
	.poll = webchat_poll,
	.send = webchat_send,
	.cleanup = webchat_cleanup,
};

const channel_t *channel_webchat_get(void)
{
	return &webchat_channel;
}
