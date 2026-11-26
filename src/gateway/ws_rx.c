#include "gateway/ws_rx.h"
#include "cJSON.h"
#include <stdlib.h>
#include <string.h>

void ws_rx_clear(ws_rx_t *rx)
{
	free(rx->data);
	*rx = (ws_rx_t){0};
}

static int ws_rx_append(ws_rx_t *rx, const void *chunk, size_t len)
{
	const size_t need = rx->len + len + 1;
	if (need > rx->capacity) {
		size_t capacity = rx->capacity * 2;
		if (capacity < need)
			capacity = need;
		if (capacity > WS_RX_JSON_MAX + 1)
			capacity = WS_RX_JSON_MAX + 1;
		char *data = realloc(rx->data, capacity);
		if (!data)
			return -1;
		rx->data = data;
		rx->capacity = capacity;
	}
	memcpy(rx->data + rx->len, chunk, len);
	rx->len += len;
	rx->data[rx->len] = '\0';
	return 0;
}

static int ws_rx_take_text(const ws_rx_t *rx, char **text_out)
{
	int result = 0;
	const char *end = NULL;
	cJSON *root = cJSON_ParseWithLengthOpts(rx->data, rx->len + 1, &end, 1);
	if (root && end == rx->data + rx->len) {
		const char *type = cJSON_GetStringValue(cJSON_GetObjectItem(root, "type"));
		const char *text = cJSON_GetStringValue(cJSON_GetObjectItem(root, "text"));
		const size_t len = text ? strlen(text) : 0;
		if (type && strcmp(type, "message") == 0 && len > 0 && len <= WS_TEXT_MAX) {
			*text_out = malloc(len + 1);
			if (*text_out) {
				memcpy(*text_out, text, len + 1);
				result = 1;
			} else {
				result = -1;
			}
		}
	}
	cJSON_Delete(root);
	return result;
}

int ws_rx_feed(ws_rx_t *rx, const void *chunk, size_t len, int final, char **text_out)
{
	*text_out = NULL;
	if (rx->discarding) {
		rx->discarding = !final;
		return 0;
	}
	if (len > WS_RX_JSON_MAX - rx->len) {
		ws_rx_clear(rx);
		rx->discarding = !final;
		return 0;
	}
	if (len && ws_rx_append(rx, chunk, len) != 0)
		return -1;
	if (!final || !rx->len)
		return 0;
	const int result = ws_rx_take_text(rx, text_out);
	ws_rx_clear(rx);
	return result;
}
