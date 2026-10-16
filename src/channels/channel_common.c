/**
 * @file channel_common.c
 * @brief Shared channel utilities: channel_incoming_msg_clear, channel registry.
 */

#include "channels/channel.h"
#include <stdlib.h>
#include <string.h>

static struct {
	const char *name;
	const channel_t *ch;
} g_registered[MAX_REGISTERED_CHANNELS];
static int g_registered_count;

void channel_register(const char *name, const channel_t *ch)
{
	if (!name || !ch || g_registered_count >= MAX_REGISTERED_CHANNELS) return;
	g_registered[g_registered_count].name = name;
	g_registered[g_registered_count].ch = ch;
	g_registered_count++;
}

const channel_t *channel_get_by_name(const char *name)
{
	if (!name) return NULL;
	for (int i = 0; i < g_registered_count; i++) {
		if (strcmp(g_registered[i].name, name) == 0)
			return g_registered[i].ch;
	}
	return NULL;
}

size_t channel_outbound_chunk_len(const char *text, size_t max_bytes)
{
	size_t len;
	size_t n;

	if (!text || max_bytes == 0)
		return 0;
	len = strlen(text);
	if (len <= max_bytes)
		return len;
	/* Walk back over UTF-8 continuation bytes so the cut sits on a boundary. */
	n = max_bytes;
	while (n > 0 && ((unsigned char)text[n] & 0xC0) == 0x80)
		n--;
	return n > 0 ? n : max_bytes;
}

void channel_incoming_msg_clear(channel_incoming_msg_t *msg)
{
	if (!msg) return;
	free(msg->session_id);
	msg->session_id = NULL;
	free(msg->user_id);
	msg->user_id = NULL;
	free(msg->text);
	msg->text = NULL;
	if (msg->attachments) {
		for (size_t i = 0; i < msg->attachments_count; i++)
			free(msg->attachments[i].path_or_base64);
		free(msg->attachments);
		msg->attachments = NULL;
		msg->attachments_count = 0;
	}
}
