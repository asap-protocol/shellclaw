/**
 * @file channel_common.c
 * @brief Shared channel utilities: chunked send, incoming msg clear, registry.
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

int channel_send_chunked(const char *text, size_t max_bytes,
                         channel_chunk_post_fn post, void *ctx,
                         channel_chunk_fail_t *fail)
{
	char *piece;
	const char *cursor;
	size_t index;

	if (fail) {
		fail->index = 0;
		fail->offset = 0;
	}
	if (!text || max_bytes == 0 || !post)
		return -1;
	/* One reused, always-NUL-terminated piece buffer: platform posts take
	 * plain C strings, so the chunk copy lives here instead of per caller. */
	piece = malloc(max_bytes + 1);
	if (!piece)
		return -1;
	cursor = text;
	index = 0;
	while (cursor[0] != '\0') {
		size_t n = channel_outbound_chunk_len(cursor, max_bytes);

		if (n == 0 || n > max_bytes) {
			/* Fail-safe only: chunk_len guarantees 0 < n <= max_bytes
			 * for non-empty cursor, so a future bounds bug stops the
			 * send instead of walking past the piece buffer. */
			if (fail) {
				fail->index = index;
				fail->offset = (size_t)(cursor - text);
			}
			free(piece);
			return -1;
		}
		memcpy(piece, cursor, n);
		piece[n] = '\0';
		if (post(piece, ctx) != 0) {
			if (fail) {
				fail->index = index;
				fail->offset = (size_t)(cursor - text);
			}
			free(piece);
			return -1;
		}
		cursor += n;
		index++;
	}
	free(piece);
	return 0;
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
