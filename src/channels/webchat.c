/**
 * @file webchat.c
 * @brief WebChat channel: poll from WS queue, send to WS connection.
 */
#define _POSIX_C_SOURCE 200809L

#include "channels/channel.h"
#include "channels/webchat.h"
#include "gateway/ws.h"
#include "core/config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WEBCHAT_USER_ID "webchat"

static int webchat_init(const config_t *cfg)
{
	(void)cfg;
	return 0;
}

static int webchat_poll(channel_incoming_msg_t *out, int timeout_ms)
{
	if (!out) return -1;
	memset(out, 0, sizeof(*out));
	char session_id[128];
	char text[WS_TEXT_MAX];
	int r = ws_pop_incoming(session_id, sizeof(session_id), text, sizeof(text), timeout_ms);
	if (r != 1) return r;
	out->session_id = strdup(session_id);
	out->user_id = strdup(WEBCHAT_USER_ID);
	out->text = strdup(text);
	out->attachments = NULL;
	out->attachments_count = 0;
	if (!out->session_id || !out->user_id || !out->text) {
		free(out->session_id);
		free(out->user_id);
		free(out->text);
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
	return ws_send_to(recipient, text);
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
