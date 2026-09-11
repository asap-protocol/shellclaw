/**
 * @file test_shell.c
 * @brief Unit tests for shell tool: safe commands, blocklist, timeout.
 */

#include "tools/tool.h"
#include "tools/shell.h"
#include "core/config.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
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

static void test_shell_ls_succeeds(void)
{
	const tool_t *t = tool_shell_get();
	tool_shell_set_config(NULL);
	char buf[4096];
	int r = t->execute("{\"command\":\"ls\"}", buf, sizeof(buf));
	MU_ASSERT(r == 0, "ls returns 0");
	MU_ASSERT(strlen(buf) > 0, "ls returns output");
}

static void test_shell_blocked_rm_rf(void)
{
	const tool_t *t = tool_shell_get();
	char buf[256];
	buf[0] = '\0';
	(void)t->execute("{\"command\":\"rm -rf /\"}", buf, sizeof(buf));
	MU_ASSERT(strstr(buf, "blocked") != NULL, "rm -rf / blocked");
}

static void test_shell_blocked_mkfs(void)
{
	const tool_t *t = tool_shell_get();
	char buf[256];
	buf[0] = '\0';
	(void)t->execute("{\"command\":\"mkfs\"}", buf, sizeof(buf));
	MU_ASSERT(strstr(buf, "blocked") != NULL, "mkfs blocked");
}

static void test_shell_invalid_json(void)
{
	const tool_t *t = tool_shell_get();
	char buf[256];
	int r = t->execute("invalid", buf, sizeof(buf));
	MU_ASSERT(r == -1, "invalid JSON returns -1");
	MU_ASSERT(strstr(buf, "error") != NULL, "error in output");
}

static void test_shell_missing_command(void)
{
	const tool_t *t = tool_shell_get();
	char buf[256];
	int r = t->execute("{\"x\":1}", buf, sizeof(buf));
	MU_ASSERT(r == -1, "missing command returns -1");
}

static void output_cap_hang_watchdog(int sig)
{
	(void)sig;
	fprintf(stderr, "FAIL: unsandboxed shell hung after filling the output cap\n");
	_exit(2);
}

static int buf_has_nul(const char *buf, size_t n)
{
	size_t i;

	for (i = 0; i < n; i++) {
		if (buf[i] == '\0')
			return 1;
	}
	return 0;
}

static void test_shell_caps_output_without_hanging(void)
{
	const tool_t *t = tool_shell_get();
	char buf[64];
	int r;

	tool_shell_set_config(NULL);
	memset(buf, 'B', sizeof(buf));
	signal(SIGALRM, output_cap_hang_watchdog);
	alarm(5);
	/* Fill the 64-byte cap, then sleep so the child stays alive without
	 * writing (SIGPIPE will not reap it). Unsandboxed waitpid used to block
	 * forever on this path.
	 */
	r = t->execute("{\"command\":\"printf '%080d' 0; sleep 9999\"}", buf,
	               sizeof(buf));
	alarm(0);
	signal(SIGALRM, SIG_DFL);
	MU_ASSERT(r == 0, "capped shell command returns");
	MU_ASSERT(buf_has_nul(buf, sizeof(buf)), "capped output is NUL-terminated");
}

int main(void)
{
	MU_RUN(test_shell_blocked_rm_rf);
	MU_RUN(test_shell_blocked_mkfs);
	MU_RUN(test_shell_ls_succeeds);
	MU_RUN(test_shell_invalid_json);
	MU_RUN(test_shell_missing_command);
	MU_RUN(test_shell_caps_output_without_hanging);
	printf("%d tests run, %d failed\n", tests_run, tests_failed);
	return tests_failed ? 1 : 0;
}
