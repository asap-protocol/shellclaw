/**
 * @file test_cli.c
 * @brief Unit tests for CLI channel: one-shot, poll, send.
 */

#include "channels/channel.h"
#include "core/config.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>

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

#define MU_RUN(test) do { test(); } while (0)

static void test_cli_channel_exists(void)
{
	const channel_t *ch = channel_cli_get();
	MU_ASSERT(ch != NULL, "channel_cli_get returns non-NULL");
	MU_ASSERT(ch->name != NULL && strcmp(ch->name, "cli") == 0, "cli name is 'cli'");
	MU_ASSERT(ch->init != NULL && ch->poll != NULL && ch->send != NULL && ch->cleanup != NULL,
	          "all vtable pointers set");
}

static void test_cli_one_shot_returns_message(void)
{
	const channel_t *ch = channel_cli_get();
	channel_cli_set_one_shot("test message");
	MU_ASSERT(ch->init(NULL) == 0, "cli init succeeds");
	channel_incoming_msg_t msg;
	memset(&msg, 0, sizeof(msg));
	int r = ch->poll(&msg, 0);
	MU_ASSERT(r == 1, "poll returns 1 (message)");
	MU_ASSERT(msg.session_id != NULL && strstr(msg.session_id, "cli") != NULL, "session_id contains cli");
	MU_ASSERT(msg.text != NULL && strcmp(msg.text, "test message") == 0, "text matches one-shot");
	channel_incoming_msg_clear(&msg);
	ch->cleanup();
}

static void test_cli_one_shot_consumed_after_poll(void)
{
	const channel_t *ch = channel_cli_get();
	channel_cli_set_one_shot("once");
	ch->init(NULL);
	channel_incoming_msg_t msg;
	memset(&msg, 0, sizeof(msg));
	ch->poll(&msg, 0);
	channel_incoming_msg_clear(&msg);
	memset(&msg, 0, sizeof(msg));
	int r = ch->poll(&msg, 0);
	MU_ASSERT(r == 0, "second poll returns 0 (one-shot consumed)");
	channel_incoming_msg_clear(&msg);
	ch->cleanup();
}

static void test_cli_piped_input_keeps_the_last_line(void)
{
	const channel_t *ch = channel_cli_get();
	channel_incoming_msg_t msg = {0};
	const char input[] = "first\nlast";
	int input_pipe[2];
	int saved_stdin = dup(STDIN_FILENO);

	MU_ASSERT(pipe(input_pipe) == 0, "open input pipe");
	MU_ASSERT(write(input_pipe[1], input, sizeof(input) - 1) == sizeof(input) - 1, "write both lines");
	close(input_pipe[1]);
	MU_ASSERT(dup2(input_pipe[0], STDIN_FILENO) == STDIN_FILENO, "replace stdin");
	if (input_pipe[0] != STDIN_FILENO)
		close(input_pipe[0]);

	ch->init(NULL);
	int first_result = ch->poll(&msg, 0);
	int first_matches = msg.text && strcmp(msg.text, "first") == 0;
	channel_incoming_msg_clear(&msg);
	int last_result = ch->poll(&msg, 0);
	int last_matches = msg.text && strcmp(msg.text, "last") == 0;
	channel_incoming_msg_clear(&msg);
	int closed = ch->poll(&msg, 0);
	ch->cleanup();

	if (saved_stdin >= 0) {
		dup2(saved_stdin, STDIN_FILENO);
		close(saved_stdin);
	} else {
		close(STDIN_FILENO);
	}
	MU_ASSERT(first_result == 1 && first_matches, "read the first piped line");
	MU_ASSERT(last_result == 1 && last_matches, "read the final line without a newline");
	MU_ASSERT(closed == CHANNEL_POLL_CLOSED, "report closed input");
}

static void test_cli_send_succeeds(void)
{
	const channel_t *ch = channel_cli_get();
	ch->init(NULL);
	int r = ch->send("cli:default", "Hello", NULL, 0);
	MU_ASSERT(r == 0, "send returns 0");
	ch->cleanup();
}

int main(void)
{
	MU_RUN(test_cli_channel_exists);
	MU_RUN(test_cli_one_shot_returns_message);
	MU_RUN(test_cli_one_shot_consumed_after_poll);
	MU_RUN(test_cli_send_succeeds);
	MU_RUN(test_cli_piped_input_keeps_the_last_line);
	printf("%d tests run, %d failed\n", tests_run, tests_failed);
	return tests_failed ? 1 : 0;
}
