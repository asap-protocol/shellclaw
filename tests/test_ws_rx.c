/**
 * @file test_ws_rx.c
 * @brief WebChat RX reassembly across libwebsockets spills (no libwebsockets).
 */
#define _POSIX_C_SOURCE 200809L

#include "gateway/ws.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
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

static char *json_message(const char *text, size_t *wire_len)
{
	const char *prefix = "{\"type\":\"message\",\"text\":\"";
	size_t n = strlen(text);
	size_t i;
	size_t j;
	char *wire = malloc(n * 2 + 64);

	if (!wire)
		return NULL;
	j = strlen(prefix);
	memcpy(wire, prefix, j);
	for (i = 0; i < n; i++) {
		if (text[i] == '\\' || text[i] == '"')
			wire[j++] = '\\';
		wire[j++] = text[i];
	}
	wire[j++] = '"';
	wire[j++] = '}';
	wire[j] = '\0';
	*wire_len = j;
	return wire;
}

static int feed_spills(ws_rx_acc_t *acc, const char *wire, size_t wire_len, char **got)
{
	size_t off = 0;
	int rc = 0;

	*got = NULL;
	while (off < wire_len) {
		size_t n = wire_len - off;
		int is_final;
		char *part = NULL;

		if (n > WS_RX_SPILL)
			n = WS_RX_SPILL;
		is_final = off + n == wire_len;
		rc = ws_rx_acc_feed(acc, wire + off, n, is_final, &part);
		off += n;
		if (!is_final) {
			if (rc != 0 || part != NULL)
				return -1;
			continue;
		}
		*got = part;
	}
	return rc;
}

/** A max-length body of backslashes expands past one RX spill. */
static int test_escaped_max_rejoins_across_spills(void)
{
	ws_rx_acc_t acc;
	char *text;
	char *wire;
	char *got = NULL;
	size_t wire_len = 0;
	int rc;

	text = malloc((size_t)WS_TEXT_MAX + 1);
	ASSERT(text != NULL);
	memset(text, '\\', (size_t)WS_TEXT_MAX);
	text[WS_TEXT_MAX] = '\0';
	wire = json_message(text, &wire_len);
	ASSERT(wire != NULL);
	ASSERT(wire_len > WS_RX_SPILL);
	memset(&acc, 0, sizeof(acc));
	rc = feed_spills(&acc, wire, wire_len, &got);
	ASSERT(rc == 1);
	ASSERT(got != NULL);
	ASSERT(strcmp(got, text) == 0);
	ASSERT(acc.buf == NULL);
	free(got);
	free(wire);
	free(text);
	ws_rx_acc_free(&acc);
	return 0;
}

static int test_oversize_frame_does_not_poison_next(void)
{
	ws_rx_acc_t acc;
	char *huge;
	char *got = NULL;
	const char *ok = "{\"type\":\"message\",\"text\":\"hi\"}";

	memset(&acc, 0, sizeof(acc));
	huge = malloc(WS_RX_JSON_MAX + 1);
	ASSERT(huge != NULL);
	memset(huge, '{', WS_RX_JSON_MAX + 1);
	ASSERT(ws_rx_acc_feed(&acc, huge, WS_RX_JSON_MAX + 1, 0, &got) == 0);
	ASSERT(got == NULL);
	ASSERT(ws_rx_acc_feed(&acc, "rest", 4, 1, &got) == 0);
	ASSERT(got == NULL);
	ASSERT(ws_rx_acc_feed(&acc, ok, strlen(ok), 1, &got) == 1);
	ASSERT(got != NULL && strcmp(got, "hi") == 0);
	free(got);
	free(huge);
	ws_rx_acc_free(&acc);
	return 0;
}

static int test_text_past_ws_text_max_is_dropped(void)
{
	ws_rx_acc_t acc;
	char *text;
	char *wire;
	char *got = NULL;
	size_t wire_len = 0;

	text = malloc((size_t)WS_TEXT_MAX + 2);
	ASSERT(text != NULL);
	memset(text, 'a', (size_t)WS_TEXT_MAX + 1);
	text[WS_TEXT_MAX + 1] = '\0';
	wire = json_message(text, &wire_len);
	ASSERT(wire != NULL);
	memset(&acc, 0, sizeof(acc));
	ASSERT(feed_spills(&acc, wire, wire_len, &got) == 0);
	ASSERT(got == NULL);
	ASSERT(acc.buf == NULL);
	free(wire);
	free(text);
	ws_rx_acc_free(&acc);
	return 0;
}

static int test_conn_feed_joins_and_unregister_drops_partial(void)
{
	char *got = NULL;
	const char *head = "{\"type\":\"message\",\"te";
	const char *tail = "xt\":\"ping\"}";
	const char *next = "{\"type\":\"message\",\"text\":\"next\"}";

	ws_cleanup();
	ASSERT(ws_register_conn(4, (ws_conn_t)(intptr_t)4) == 0);
	ASSERT(ws_conn_rx_feed(4, head, strlen(head), 0, &got) == 0);
	ASSERT(got == NULL);
	ASSERT(ws_conn_rx_feed(4, tail, strlen(tail), 1, &got) == 1);
	ASSERT(got != NULL && strcmp(got, "ping") == 0);
	free(got);
	got = NULL;
	ASSERT(ws_conn_rx_feed(4, head, strlen(head), 0, &got) == 0);
	ws_unregister_conn(4);
	ASSERT(ws_conn_rx_feed(4, tail, strlen(tail), 1, &got) == 0);
	ASSERT(got == NULL);
	ASSERT(ws_register_conn(4, (ws_conn_t)(intptr_t)4) == 0);
	ASSERT(ws_conn_rx_feed(4, next, strlen(next), 1, &got) == 1);
	ASSERT(got != NULL && strcmp(got, "next") == 0);
	free(got);
	ws_cleanup();
	return 0;
}

int main(void)
{
	RUN(test_escaped_max_rejoins_across_spills());
	RUN(test_oversize_frame_does_not_poison_next());
	RUN(test_text_past_ws_text_max_is_dropped());
	RUN(test_conn_feed_joins_and_unregister_drops_partial());
	printf("test_ws_rx: all tests passed\n");
	return 0;
}
