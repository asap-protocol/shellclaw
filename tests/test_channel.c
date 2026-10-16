/**
 * @file test_channel.c
 * @brief Unit tests for channel_t vtable and stub channel.
 */

#include "channels/channel.h"
#include <stdio.h>
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

int main(void)
{
	MU_RUN(test_stub_channel_exists);
	MU_RUN(test_stub_init_succeeds);
	MU_RUN(test_stub_poll_returns_no_message);
	MU_RUN(test_incoming_msg_clear_safe);
	MU_RUN(test_stub_send_succeeds);
	MU_RUN(test_stub_cleanup);
	MU_RUN(test_outbound_chunk_len);
	printf("%d tests run, %d failed\n", tests_run, tests_failed);
	return tests_failed ? 1 : 0;
}
