/**
 * @file ws_rx.c
 * @brief Reassemble one WebChat websocket message before JSON parse.
 *
 * libwebsockets spills a frame into rx_buffer_size chunks (WS_RX_SPILL)
 * and only then reports the final fragment. Parsing each chunk drops a
 * message whose escaped JSON is larger than one spill.
 */
#include "gateway/ws.h"
#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void ws_rx_acc_free(ws_rx_acc_t *acc)
{
	if (!acc)
		return;
	free(acc->buf);
	acc->buf = NULL;
	acc->len = 0;
	acc->cap = 0;
	acc->skip = 0;
}

static int ws_rx_too_big(const ws_rx_acc_t *acc, size_t chunk_len)
{
	if (chunk_len == 0)
		return 0;
	if (acc->len > WS_RX_JSON_MAX)
		return 1;
	return chunk_len > WS_RX_JSON_MAX - acc->len;
}

static int ws_rx_append(ws_rx_acc_t *acc, const void *in, size_t chunk_len)
{
	size_t need;
	char *grown;
	size_t ncap;

	if (chunk_len == 0)
		return 0;
	if (!in)
		return -1;
	need = acc->len + chunk_len + 1;
	if (acc->cap < need) {
		ncap = acc->cap ? acc->cap * 2 : 4096;
		while (ncap < need)
			ncap *= 2;
		if (ncap > WS_RX_JSON_MAX + 1)
			ncap = WS_RX_JSON_MAX + 1;
		if (ncap < need)
			return -1;
		grown = realloc(acc->buf, ncap);
		if (!grown)
			return -1;
		acc->buf = grown;
		acc->cap = ncap;
	}
	memcpy(acc->buf + acc->len, in, chunk_len);
	acc->len += chunk_len;
	acc->buf[acc->len] = '\0';
	return 0;
}

static int ws_rx_copy_message_text(const cJSON *root, char **text_out)
{
	const cJSON *type_item;
	const cJSON *text_item;
	size_t n;
	char *copy;

	type_item = cJSON_GetObjectItem(root, "type");
	text_item = cJSON_GetObjectItem(root, "text");
	if (!cJSON_IsString(type_item) || !type_item->valuestring ||
	    strcmp(type_item->valuestring, "message") != 0 ||
	    !cJSON_IsString(text_item) || !text_item->valuestring)
		return 0;
	n = strlen(text_item->valuestring);
	if (n == 0 || n > (size_t)WS_TEXT_MAX)
		return 0;
	copy = malloc(n + 1);
	if (!copy)
		return -1;
	memcpy(copy, text_item->valuestring, n + 1);
	*text_out = copy;
	return 1;
}

static int ws_rx_take_text(const ws_rx_acc_t *acc, char **text_out)
{
	cJSON *root;
	int rc;

	if (!acc->buf || acc->len == 0)
		return 0;
	root = cJSON_Parse(acc->buf);
	if (!root)
		return 0;
	rc = ws_rx_copy_message_text(root, text_out);
	cJSON_Delete(root);
	return rc;
}

static void ws_rx_drop_oversize(ws_rx_acc_t *acc, int is_final)
{
	ws_rx_acc_free(acc);
	acc->skip = is_final ? 0 : 1;
	fprintf(stderr, "shellclaw: ws: drop inbound message over %zu bytes\n",
	        WS_RX_JSON_MAX);
}

int ws_rx_acc_feed(ws_rx_acc_t *acc, const void *in, size_t len, int is_final,
                   char **text_out)
{
	int rc;

	if (!acc || !text_out)
		return -1;
	*text_out = NULL;
	if (acc->skip) {
		if (is_final)
			acc->skip = 0;
		return 0;
	}
	if (ws_rx_too_big(acc, len)) {
		ws_rx_drop_oversize(acc, is_final);
		return 0;
	}
	if (ws_rx_append(acc, in, len) != 0) {
		ws_rx_acc_free(acc);
		acc->skip = is_final ? 0 : 1;
		return -1;
	}
	if (!is_final)
		return 0;
	rc = ws_rx_take_text(acc, text_out);
	ws_rx_acc_free(acc);
	return rc;
}
