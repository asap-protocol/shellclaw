/**
 * @file test_channel.c
 * @brief Unit tests for channel_t vtable, stub channel, chunked send helper.
 */
#define _POSIX_C_SOURCE 200809L

#include "channels/channel.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int tests_run = 0;
static int tests_failed = 0;

#define MU_ASSERT(cond, msg) do { \
	tests_run++; \
	if (!(cond)) { \
		fprintf(stderr, "FAIL: %s\n", (msg)); \
		tests_failed++; \
		return; \
	} \
} while (0)

#define MU_RUN(test) do { \
	test(); \
} while (0)

static void test_stub_channel_exists(void)
{
	const channel_t *ch = channel_stub_get();
	MU_ASSERT(ch != NULL, "channel_stub_get returns non-NULL");
	MU_ASSERT(ch->name != NULL && strcmp(ch->name, "stub") == 0, "stub name is 'stub'");
	MU_ASSERT(ch->init != NULL, "init is set");
	MU_ASSERT(ch->poll != NULL, "poll is set");
	MU_ASSERT(ch->send != NULL, "send is set");
	MU_ASSERT(ch->cleanup != NULL, "cleanup is set");
}

static void test_stub_init_succeeds(void)
{
	const channel_t *ch = channel_stub_get();
	int r = ch->init(NULL);
	MU_ASSERT(r == 0, "stub init returns 0");
}

static void test_stub_poll_returns_no_message(void)
{
	const channel_t *ch = channel_stub_get();
	channel_incoming_msg_t msg;
	memset(&msg, 0, sizeof(msg));
	int r = ch->poll(&msg, 100);
	MU_ASSERT(r == 0, "stub poll returns 0 (no message)");
	channel_incoming_msg_clear(&msg);
}

static void test_incoming_msg_clear_safe(void)
{
	channel_incoming_msg_t msg;
	memset(&msg, 0, sizeof(msg));
	channel_incoming_msg_clear(&msg);
	channel_incoming_msg_clear(&msg);
	MU_ASSERT(1, "channel_incoming_msg_clear is safe on zeroed/cleared");
}

static void test_stub_send_succeeds(void)
{
	const channel_t *ch = channel_stub_get();
	int r = ch->send("recipient", "hello", NULL, 0);
	MU_ASSERT(r == 0, "stub send returns 0");
}

static void test_stub_cleanup(void)
{
	const channel_t *ch = channel_stub_get();
	ch->cleanup();
	MU_ASSERT(1, "stub cleanup does not crash");
}

static void test_outbound_chunk_len(void)
{
	/* U+20AC EURO SIGN is the three-byte sequence e2 82 ac. */
	const char euro[] = "x\xE2\x82\xAC" "y";
	size_t n;

	MU_ASSERT(channel_outbound_chunk_len(NULL, 10) == 0, "NULL text is empty");
	MU_ASSERT(channel_outbound_chunk_len("", 10) == 0, "empty text is empty");
	MU_ASSERT(channel_outbound_chunk_len("hi", 0) == 0, "zero cap is empty");
	MU_ASSERT(channel_outbound_chunk_len("hello", 5) == 5, "exact fit stays one piece");
	MU_ASSERT(channel_outbound_chunk_len("hello", 2) == 2, "ascii splits on the cap");
	n = channel_outbound_chunk_len(euro, 2);
	MU_ASSERT(n == 1, "multibyte character is not split");
	MU_ASSERT(channel_outbound_chunk_len(euro + n, 3) == 3, "next piece is the whole euro sign");
}

static void test_outbound_chunk_len_4byte_rune(void)
{
	/* U+1F600 GRINNING FACE is the four-byte sequence f0 9f 98 80. */
	const char grin[] = "xx\xF0\x9F\x98\x80" "yy";
	size_t n = channel_outbound_chunk_len(grin, 3);

	MU_ASSERT(n == 2, "4-byte code point at the split edge is not split");
	n = channel_outbound_chunk_len(grin + n, 4);
	MU_ASSERT(n == 4, "next piece is the whole 4-byte code point");
}

/* --- channel_send_chunked --- */

#define CHUNK_TEST_MAX_PIECES 64

typedef struct {
	char *pieces[CHUNK_TEST_MAX_PIECES];
	size_t count;
} chunk_recorder_t;

static int record_piece(const char *piece, void *userdata)
{
	chunk_recorder_t *rec = (chunk_recorder_t *)userdata;

	if (rec->count >= CHUNK_TEST_MAX_PIECES)
		return -1;
	rec->pieces[rec->count] = strdup(piece);
	if (!rec->pieces[rec->count])
		return -1;
	rec->count++;
	return 0;
}

static void recorder_clear(chunk_recorder_t *rec)
{
	size_t i;

	for (i = 0; i < rec->count; i++)
		free(rec->pieces[i]);
	rec->count = 0;
}

static void test_send_chunked_round_trip(void)
{
	/* ASCII, a 3-byte euro sign, and a 4-byte grinning face. */
	const char *input = "hi \xE2\x82\xAC\xF0\x9F\x98\x80 tail text!";
	char joined[256];
	size_t joined_len = 0;
	chunk_recorder_t rec;
	channel_chunk_fail_t fail;
	size_t i;

	fail.index = 99;
	fail.offset = 99;
	rec.count = 0;
	MU_ASSERT(channel_send_chunked(input, 4, record_piece, &rec, &fail) == 0,
	          "chunked send over multibyte text succeeds");
	MU_ASSERT(rec.count >= 2, "text longer than max_bytes is split");
	for (i = 0; i < rec.count; i++) {
		size_t plen;

		MU_ASSERT(rec.pieces[i] != NULL, "recorded piece is non-NULL");
		MU_ASSERT(rec.pieces[i][0] != '\0', "no piece is empty");
		plen = strlen(rec.pieces[i]);
		MU_ASSERT(joined_len + plen < sizeof(joined), "joined buffer fits");
		memcpy(joined + joined_len, rec.pieces[i], plen);
		joined_len += plen;
	}
	joined[joined_len] = '\0';
	MU_ASSERT(strcmp(joined, input) == 0, "pieces concatenate back to the input");
	MU_ASSERT(fail.index == 0 && fail.offset == 0, "fail stays zeroed on success");
	recorder_clear(&rec);

	/* Length within the cap is delivered as a single piece. */
	MU_ASSERT(channel_send_chunked(input, 128, record_piece, &rec, &fail) == 0,
	          "text within max_bytes sends as one piece");
	MU_ASSERT(rec.count == 1, "single piece for text within max_bytes");
	MU_ASSERT(rec.pieces[0] && strcmp(rec.pieces[0], input) == 0,
	          "single piece equals the input");
	recorder_clear(&rec);
}

typedef struct {
	size_t calls;
} chunk_counter_t;

static int post_fail_on_third(const char *piece, void *userdata)
{
	chunk_counter_t *counter = (chunk_counter_t *)userdata;

	(void)piece;
	counter->calls++;
	return counter->calls == 3 ? -1 : 0;
}

static void test_send_chunked_reports_failing_chunk(void)
{
	channel_chunk_fail_t fail;
	chunk_counter_t counter;
	int r;

	fail.index = 99;
	fail.offset = 99;
	counter.calls = 0;
	/* 12 bytes / max 4: chunks "abcd" (0), "efgh" (4), "ijkl" (8). */
	r = channel_send_chunked("abcdefghijkl", 4, post_fail_on_third, &counter, &fail);
	MU_ASSERT(r != 0, "send fails when a chunk post fails");
	MU_ASSERT(counter.calls == 3, "chunks before the failure are posted");
	MU_ASSERT(fail.index == 2, "failing chunk index is reported");
	MU_ASSERT(fail.offset == 8, "failing chunk byte offset is reported");
}

static void test_send_chunked_contract_edges(void)
{
	channel_chunk_fail_t fail;
	chunk_recorder_t rec;

	fail.index = 99;
	fail.offset = 99;
	rec.count = 0;
	MU_ASSERT(channel_send_chunked(NULL, 10, record_piece, &rec, &fail) != 0,
	          "NULL text fails");
	MU_ASSERT(channel_send_chunked("hi", 0, record_piece, &rec, &fail) != 0,
	          "zero max_bytes fails");
	MU_ASSERT(channel_send_chunked("hi", 10, NULL, &rec, &fail) != 0,
	          "NULL post callback fails");
	MU_ASSERT(rec.count == 0, "invalid arguments post no pieces");
	MU_ASSERT(fail.index == 0 && fail.offset == 0,
	          "fail stays zeroed without a post failure");
	/* Empty text is a no-op success: there is no chunk to post. */
	MU_ASSERT(channel_send_chunked("", 10, record_piece, &rec, &fail) == 0,
	          "empty text is a no-op success");
	MU_ASSERT(rec.count == 0, "empty text posts no pieces");
}

int main(void)
{
	MU_RUN(test_stub_channel_exists);
	MU_RUN(test_stub_init_succeeds);
	MU_RUN(test_stub_poll_returns_no_message);
	MU_RUN(test_incoming_msg_clear_safe);
	MU_RUN(test_stub_send_succeeds);
	MU_RUN(test_stub_cleanup);
	MU_RUN(test_outbound_chunk_len);
	MU_RUN(test_outbound_chunk_len_4byte_rune);
	MU_RUN(test_send_chunked_round_trip);
	MU_RUN(test_send_chunked_reports_failing_chunk);
	MU_RUN(test_send_chunked_contract_edges);
	printf("%d tests run, %d failed\n", tests_run, tests_failed);
	return tests_failed ? 1 : 0;
}
