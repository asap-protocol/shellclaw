/**
 * @file test_shell.c
 * @brief Unit tests for shell tool: safe commands, blocklist, timeout.
 */
#define _POSIX_C_SOURCE 200809L

#include "tools/tool.h"
#include "tools/shell.h"
#include "core/config.h"
#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
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

static void test_shell_blocked_auth_tokens(void)
{
	const tool_t *t = tool_shell_get();
	char buf[256];
	buf[0] = '\0';
	tool_shell_set_config(NULL);
	(void)t->execute("{\"command\":\"cat ~/.shellclaw/auth_tokens.json\"}", buf, sizeof(buf));
	MU_ASSERT(strstr(buf, "blocked") != NULL, "cat auth_tokens.json blocked");
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

static int argv0_is_sleep(const char *arg0)
{
	const char *base;
	if (!arg0 || !arg0[0])
		return 0;
	base = strrchr(arg0, '/');
	base = base ? base + 1 : arg0;
	return strcmp(base, "sleep") == 0;
}

static int cmdline_is_sleep_marker(const char *buf, size_t n, const char *marker)
{
	size_t i = 0;
	const char *arg1;
	if (n == 0 || !marker)
		return 0;
	while (i < n && buf[i] != '\0')
		i++;
	if (i >= n || i + 1 >= n)
		return 0;
	if (!argv0_is_sleep(buf))
		return 0;
	arg1 = buf + i + 1;
	return strcmp(arg1, marker) == 0;
}

static int count_sleep_argv_proc(const char *marker)
{
	DIR *dir;
	struct dirent *ent;
	int count = 0;
	dir = opendir("/proc");
	if (!dir)
		return -1;
	while ((ent = readdir(dir)) != NULL) {
		char path[64];
		char buf[256];
		ssize_t n;
		int fd;
		if (!isdigit((unsigned char)ent->d_name[0]))
			continue;
		if (snprintf(path, sizeof(path), "/proc/%s/cmdline", ent->d_name)
		    >= (int)sizeof(path))
			continue;
		fd = open(path, O_RDONLY);
		if (fd < 0)
			continue;
		n = read(fd, buf, sizeof(buf) - 1);
		close(fd);
		if (n <= 0)
			continue;
		buf[n] = '\0';
		if (cmdline_is_sleep_marker(buf, (size_t)n + 1, marker))
			count++;
	}
	closedir(dir);
	return count;
}

static int ps_line_is_sleep_marker(char *line, const char *marker)
{
	char *s = line;
	char *nl;
	char *base;
	size_t marker_len;
	nl = strchr(line, '\n');
	if (nl)
		*nl = '\0';
	while (*s == ' ' || *s == '\t')
		s++;
	base = strrchr(s, '/');
	base = base ? base + 1 : s;
	marker_len = strlen(marker);
	if (strncmp(base, "sleep ", 6) != 0)
		return 0;
	return strcmp(base + 6, marker) == 0 && marker_len > 0;
}

static int count_sleep_argv_ps(const char *marker)
{
	FILE *fp;
	char line[256];
	int count = 0;
	fp = popen("ps -axo args=", "r");
	if (!fp)
		return -1;
	while (fgets(line, sizeof(line), fp) != NULL) {
		if (ps_line_is_sleep_marker(line, marker))
			count++;
	}
	(void)pclose(fp);
	return count;
}

/* Linux CI: /proc cmdline. Darwin (no /proc): ps args. argv0 must be sleep. */
static int count_sleep_argv(const char *marker)
{
	int n = count_sleep_argv_proc(marker);
	if (n >= 0)
		return n;
	return count_sleep_argv_ps(marker);
}

static int sleep_argv_did_not_grow(const char *marker, int before)
{
	struct timespec ts;
	int i;
	for (i = 0; i < 20; i++) {
		int now = count_sleep_argv(marker);
		if (now >= 0 && now <= before)
			return 1;
		ts.tv_sec = 0;
		ts.tv_nsec = 50 * 1000 * 1000;
		(void)nanosleep(&ts, NULL);
	}
	return 0;
}

static void test_shell_caps_output_without_hanging(void)
{
	const tool_t *t = tool_shell_get();
	char buf[64];
	int r;
	int before;
	tool_shell_set_config(NULL);
	memset(buf, 'B', sizeof(buf));
	before = count_sleep_argv("9999");
	MU_ASSERT(before >= 0, "can count sleep 9999 processes");
	signal(SIGALRM, output_cap_hang_watchdog);
	alarm(5);
	/* Fill the 64-byte cap, then sleep so the child stays alive without
	 * writing (SIGPIPE will not reap it). Unsandboxed waitpid used to block
	 * forever on this path (#69).
	 */
	r = t->execute("{\"command\":\"printf '%080d' 0; sleep 9999\"}", buf,
	               sizeof(buf));
	alarm(0);
	signal(SIGALRM, SIG_DFL);
	MU_ASSERT(r == 0, "capped shell command returns");
	MU_ASSERT(buf[0] == '0', "capped output starts with truncated zeros");
	MU_ASSERT(buf_has_nul(buf, sizeof(buf)), "capped output is NUL-terminated");
	MU_ASSERT(sleep_argv_did_not_grow("9999", before),
	          "sequential sleep 9999 did not leak");
}

static void test_shell_caps_output_kills_background_sleep(void)
{
	const tool_t *t = tool_shell_get();
	char buf[64];
	int r;
	int before;
	tool_shell_set_config(NULL);
	memset(buf, 'B', sizeof(buf));
	before = count_sleep_argv("9998");
	MU_ASSERT(before >= 0, "can count sleep 9998 processes");
	signal(SIGALRM, output_cap_hang_watchdog);
	alarm(5);
	/* Shell can exit after printf while the background sleep stays in the
	 * process group. Skipping kill_command_tree when waitpid already reaped
	 * the shell leaked that grandchild (#96).
	 */
	r = t->execute("{\"command\":\"trap '' HUP; sleep 9998 & printf '%080d' 0\"}",
	               buf, sizeof(buf));
	alarm(0);
	signal(SIGALRM, SIG_DFL);
	MU_ASSERT(r == 0, "background capped shell command returns");
	MU_ASSERT(buf[0] == '0', "background capped output is truncated zeros");
	MU_ASSERT(buf_has_nul(buf, sizeof(buf)), "background capped output is NUL-terminated");
	MU_ASSERT(sleep_argv_did_not_grow("9998", before),
	          "background sleep 9998 did not leak");
}

int main(void)
{
	MU_RUN(test_shell_blocked_rm_rf);
	MU_RUN(test_shell_blocked_mkfs);
	MU_RUN(test_shell_blocked_auth_tokens);
	MU_RUN(test_shell_ls_succeeds);
	MU_RUN(test_shell_invalid_json);
	MU_RUN(test_shell_missing_command);
	MU_RUN(test_shell_caps_output_without_hanging);
	MU_RUN(test_shell_caps_output_kills_background_sleep);
	printf("%d tests run, %d failed\n", tests_run, tests_failed);
	return tests_failed ? 1 : 0;
}
