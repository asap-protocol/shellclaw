#include "gateway/ws_rx.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do { \
	if (!(condition)) { \
		fprintf(stderr, "%s:%d: %s failed\n", __FILE__, __LINE__, #condition); \
		exit(EXIT_FAILURE); \
	} \
} while (0)

static const char prefix[] = "{\"type\":\"message\",\"text\":\"";
static const char short_message[] = "{\"type\":\"message\",\"text\":\"ready\"}";

static void test_escaped_text_spans_chunks(void)
{
	ws_rx_t rx = {0};
	char *text = NULL;
	const size_t wire_size = sizeof(prefix) - 1 + WS_TEXT_MAX * 2 + 2;
	const size_t split = WS_RX_BUFFER_SIZE - 1;
	char *wire = malloc(wire_size);
	CHECK(wire);
	memcpy(wire, prefix, sizeof(prefix) - 1);
	memset(wire + sizeof(prefix) - 1, '\\', WS_TEXT_MAX * 2);
	memcpy(wire + wire_size - 2, "\"}", 2);
	CHECK(ws_rx_feed(&rx, wire, split, 0, &text) == 0 && !text);
	CHECK(ws_rx_feed(&rx, wire + split, wire_size - split, 1, &text) == 1);
	CHECK(strlen(text) == WS_TEXT_MAX && strspn(text, "\\") == WS_TEXT_MAX);
	CHECK(!rx.data);
	free(text);
	free(wire);
}

static void test_wire_limit_recovers_at_end_of_message(void)
{
	ws_rx_t rx = {0};
	char *text = NULL;
	char *chunk = malloc(WS_RX_BUFFER_SIZE);
	CHECK(chunk);
	memset(chunk, ' ', WS_RX_BUFFER_SIZE);
	for (size_t i = 0; i <= WS_RX_JSON_MAX / WS_RX_BUFFER_SIZE; i++)
		CHECK(ws_rx_feed(&rx, chunk, WS_RX_BUFFER_SIZE, 0, &text) == 0 && !text);
	CHECK(!rx.data);
	CHECK(ws_rx_feed(&rx, short_message, sizeof(short_message) - 1, 1, &text) == 0 && !text);
	CHECK(ws_rx_feed(&rx, short_message, sizeof(short_message) - 1, 1, &text) == 1);
	CHECK(strcmp(text, "ready") == 0);
	free(text);
	free(chunk);
}

static void test_decoded_text_limit(void)
{
	ws_rx_t rx = {0};
	char *text = NULL;
	const size_t wire_size = sizeof(prefix) - 1 + WS_TEXT_MAX + 1 + 2;
	char *wire = malloc(wire_size);
	CHECK(wire);
	memcpy(wire, prefix, sizeof(prefix) - 1);
	memset(wire + sizeof(prefix) - 1, 'a', WS_TEXT_MAX + 1);
	memcpy(wire + wire_size - 2, "\"}", 2);
	CHECK(ws_rx_feed(&rx, wire, wire_size, 1, &text) == 0 && !text);
	CHECK(!rx.data);
	free(wire);
}

int main(void)
{
	test_escaped_text_spans_chunks();
	test_wire_limit_recovers_at_end_of_message();
	test_decoded_text_limit();
	printf("test_ws_rx: all tests passed\n");
	return 0;
}
