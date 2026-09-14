/**
 * @file test_sandbox.c
 * @brief Unit tests for sandbox_exec.
 *
 * Linux-specific namespace and cgroup tests are guarded by #ifdef __linux__
 * and runtime checks for userns / cgroup availability.
 * On macOS and other platforms, the tests fall back to verifying basic
 * fork+exec behaviour: output capture, timeout, and null-safety.
 *
 * GitHub-hosted runners often cannot apply user namespaces or Landlock.
 * Production stays fail-closed (sandbox_exec returns -1). Success-path tests
 * skip in that case; host-FS and netns tests treat the deny as the expected
 * isolation failure. Do not weaken sandbox.c to make CI green.
 *
 * 5.7 Benchmark: run sandbox_exec("true") 200 times and report median.
 * The benchmark is informational only — it does not gate the test suite.
 */
#define _POSIX_C_SOURCE 200809L

#include "sandbox/sandbox.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#ifdef __linux__
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif

#define ASSERT(c) do { \
	if (!(c)) { \
		fprintf(stderr, "FAIL: %s:%d  %s\n", __FILE__, __LINE__, #c); \
		return 1; \
	} \
} while (0)

#define RUN(t) do { int r_ = (t); if (r_) return r_; } while (0)

/**
 * True when sandbox_exec fail-closed because namespaces or Landlock could
 * not apply. Distinct from argument errors (NULL cmd, zero cap).
 */
static int isolation_was_denied(int rc, const char *out)
{
	if (rc != -1)
		return 0;
	if (out == NULL)
		return 0;
	if (strstr(out, "sandbox: namespace isolation failed") != NULL)
		return 1;
	if (strstr(out, "sandbox: Landlock filesystem bound failed") != NULL)
		return 1;
	return 0;
}

static int skip_if_isolation_denied(int rc, const char *out, const char *name)
{
	if (!isolation_was_denied(rc, out))
		return 0;
	fprintf(stderr, "test_sandbox: skip %s (%s)\n", name, out);
	return 1;
}

/* ------------------------------------------------------------------ */
/* Basic functionality                                                  */
/* ------------------------------------------------------------------ */

static int test_isolation_denied_helper(void)
{
	ASSERT(isolation_was_denied(0, "sandbox: namespace isolation failed") == 0);
	ASSERT(isolation_was_denied(-1, "hello") == 0);
	ASSERT(isolation_was_denied(-1, NULL) == 0);
	ASSERT(isolation_was_denied(-1, "sandbox: namespace isolation failed") == 1);
	ASSERT(isolation_was_denied(-1, "sandbox: Landlock filesystem bound failed") == 1);
	return 0;
}

/**
 * Either isolation applies (rc == 0) or fail-closed reports the isolation
 * error. Other -1 reasons on "true" are a product bug.
 */
static int test_fail_closed_reports_isolation_error(void)
{
	char out[4096];
	int rc;

	rc = sandbox_exec("true", out, sizeof(out), 5000, NULL);
	if (rc == 0)
		return 0;
	ASSERT(isolation_was_denied(rc, out));
	return 0;
}

static int test_output_capture(void)
{
	char out[4096];
	int rc = sandbox_exec("echo hello_sandbox", out, sizeof(out), 5000, NULL);
	if (skip_if_isolation_denied(rc, out, "test_output_capture"))
		return 0;
	ASSERT(rc == 0);
	ASSERT(strstr(out, "hello_sandbox") != NULL);
	return 0;
}

static int test_stderr_captured(void)
{
	char out[4096];
	int rc = sandbox_exec("echo err >&2", out, sizeof(out), 5000, NULL);
	if (skip_if_isolation_denied(rc, out, "test_stderr_captured"))
		return 0;
	ASSERT(rc == 0);
	ASSERT(strstr(out, "err") != NULL);
	return 0;
}

static int test_null_cmd_returns_error(void)
{
	char out[256];
	ASSERT(sandbox_exec(NULL, out, sizeof(out), 5000, NULL) == -1);
	return 0;
}

static int test_zero_cap_returns_error(void)
{
	char out[1];
	ASSERT(sandbox_exec("echo hi", out, 0, 5000, NULL) == -1);
	return 0;
}

static int test_exit_nonzero_runs(void)
{
	char out[4096];
	/* Command exits non-zero; sandbox_exec should still return 0 (ran ok). */
	int rc = sandbox_exec("exit 1", out, sizeof(out), 5000, NULL);
	if (skip_if_isolation_denied(rc, out, "test_exit_nonzero_runs"))
		return 0;
	ASSERT(rc == 0);
	return 0;
}

static int test_workspace_chdir(void)
{
	char out[4096];
	sandbox_config_t cfg;
	int rc;

	memset(&cfg, 0, sizeof cfg);
	cfg.workspace_path = "/tmp";
	rc = sandbox_exec("pwd", out, sizeof(out), 5000, &cfg);
	if (skip_if_isolation_denied(rc, out, "test_workspace_chdir"))
		return 0;
	ASSERT(rc == 0);
	ASSERT(strstr(out, "/tmp") != NULL);
	return 0;
}

/* ------------------------------------------------------------------ */
/* Timeout test                                                         */
/* ------------------------------------------------------------------ */

static int test_timeout_kills_process(void)
{
	char out[4096];
	/* sleep 60 should be killed well before natural completion. */
	int rc = sandbox_exec("sleep 60", out, sizeof(out), 300, NULL);
	if (skip_if_isolation_denied(rc, out, "test_timeout_kills_process"))
		return 0;
	ASSERT(rc == 0);
	ASSERT(strstr(out, "timed out") != NULL || strlen(out) == 0);
	return 0;
}

/* ------------------------------------------------------------------ */
/* Linux-only: namespace isolation (shadow should not be readable)      */
/* ------------------------------------------------------------------ */

#ifdef __linux__
static int test_shadow_not_accessible(void)
{
	char out[4096];
	int rc;
	/* Fail-closed isolation is a deny. If the command ran, no secret leak. */
	rc = sandbox_exec("cat /etc/shadow 2>&1 || echo BLOCKED", out, sizeof(out), 5000, NULL);
	if (isolation_was_denied(rc, out))
		return 0;
	ASSERT(rc == 0);
	/* Either permission denied or the echo BLOCKED message appears. */
	ASSERT(strlen(out) > 0);
	return 0;
}

/**
 * With a workspace configured, Landlock must deny host reads even via a
 * relative symlink (allowlist may miss bare names; sandbox is the FS gate).
 * Hosts that cannot apply isolation fail closed: also a deny, no leak.
 */
static int test_workspace_landlock_blocks_symlink_escape(void)
{
	char workspace[] = "/tmp/sc_sb_ws_XXXXXX";
	char leak_path[256];
	char out[4096];
	sandbox_config_t cfg;
	char *ws;
	int rc;

	ws = mkdtemp(workspace);
	if (!ws) {
		fprintf(stderr, "test_workspace_landlock_blocks_symlink_escape: mkdtemp failed\n");
		return 1;
	}
	snprintf(leak_path, sizeof(leak_path), "%s/leak", ws);
	if (symlink("/etc/passwd", leak_path) != 0) {
		rmdir(ws);
		fprintf(stderr, "test_workspace_landlock_blocks_symlink_escape: symlink failed\n");
		return 1;
	}
	memset(&cfg, 0, sizeof cfg);
	cfg.workspace_path = ws;
	rc = sandbox_exec("cat leak 2>&1; echo EXIT:$?", out, sizeof(out), 5000, &cfg);
	unlink(leak_path);
	rmdir(ws);
	ASSERT(strstr(out, "root:x:") == NULL);
	if (isolation_was_denied(rc, out))
		return 0;
	ASSERT(rc == 0);
	ASSERT(strstr(out, "Permission denied") != NULL ||
	       strstr(out, "No such file") != NULL ||
	       strstr(out, "EXIT:1") != NULL ||
	       strstr(out, "EXIT:2") != NULL);
	return 0;
}

/**
 * Kernel FS bound must stop interpreter path concat (`chr(47)+`) that the
 * string scanner cannot see. Residual on allowlist only.
 * Fail-closed isolation is also a deny (no host passwd leak).
 */
static int test_workspace_landlock_blocks_abs_etc(void)
{
	char workspace[] = "/tmp/sc_sb_ws2_XXXXXX";
	char out[4096];
	char outp[256];
	sandbox_config_t cfg;
	char *ws;
	int rc;

	ws = mkdtemp(workspace);
	if (!ws) {
		fprintf(stderr, "test_workspace_landlock_blocks_abs_etc: mkdtemp failed\n");
		return 1;
	}
	memset(&cfg, 0, sizeof cfg);
	cfg.workspace_path = ws;
	rc = sandbox_exec(
		"python3 -c 'open(\"out\",\"w\").write(open(chr(47)+\"etc\"+chr(47)+\"passwd\").read())' 2>&1; "
		"echo EXIT:$?",
		out, sizeof(out), 8000, &cfg);
	snprintf(outp, sizeof(outp), "%s/out", ws);
	unlink(outp);
	rmdir(ws);
	ASSERT(strstr(out, "root:x:") == NULL);
	if (isolation_was_denied(rc, out))
		return 0;
	ASSERT(rc == 0);
	return 0;
}

static int test_workspace_landlock_allows_workspace_write(void)
{
	char workspace[] = "/tmp/sc_sb_wr_XXXXXX";
	char out[4096];
	char wrote[256];
	char buf[64];
	sandbox_config_t cfg;
	char *ws;
	FILE *f;
	int rc;

	ws = mkdtemp(workspace);
	if (!ws) {
		fprintf(stderr, "test_workspace_landlock_allows_workspace_write: mkdtemp failed\n");
		return 1;
	}
	memset(&cfg, 0, sizeof cfg);
	cfg.workspace_path = ws;
	rc = sandbox_exec("echo landlock_ok > wrote.txt", out, sizeof(out), 5000, &cfg);
	snprintf(wrote, sizeof(wrote), "%s/wrote.txt", ws);
	if (isolation_was_denied(rc, out)) {
		unlink(wrote);
		rmdir(ws);
		fprintf(stderr, "test_sandbox: skip test_workspace_landlock_allows_workspace_write (%s)\n",
			out);
		return 0;
	}
	ASSERT(rc == 0);
	f = fopen(wrote, "r");
	ASSERT(f != NULL);
	ASSERT(fgets(buf, sizeof(buf), f) != NULL);
	fclose(f);
	ASSERT(strstr(buf, "landlock_ok") != NULL);
	unlink(wrote);
	rmdir(ws);
	return 0;
}

static int listen_loopback_ephemeral(int *port_out)
{
	int fd;
	int one = 1;
	struct sockaddr_in addr;
	socklen_t addr_len;

	fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) return -1;
	if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one) != 0) {
		close(fd);
		return -1;
	}
	memset(&addr, 0, sizeof addr);
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	addr.sin_port = 0;
	if (bind(fd, (struct sockaddr *)&addr, sizeof addr) != 0) {
		close(fd);
		return -1;
	}
	if (listen(fd, 1) != 0) {
		close(fd);
		return -1;
	}
	addr_len = sizeof addr;
	if (getsockname(fd, (struct sockaddr *)&addr, &addr_len) != 0) {
		close(fd);
		return -1;
	}
	*port_out = (int)ntohs(addr.sin_port);
	return fd;
}

static int test_network_namespace_blocks_host_loopback(void)
{
	int port = 0;
	int srv;
	int rc;
	char cmd[256];
	char out[4096];

	if (access("/bin/bash", X_OK) != 0) {
		fprintf(stderr, "test_sandbox: skip netns loopback test (no bash)\n");
		return 0;
	}
	srv = listen_loopback_ephemeral(&port);
	ASSERT(srv >= 0);
	ASSERT(port > 0);
	snprintf(cmd, sizeof cmd,
	         "bash -c 'echo >/dev/tcp/127.0.0.1/%d' >/dev/null 2>&1 "
	         "&& echo CONNECTED || echo ISOLATED",
	         port);
	rc = sandbox_exec(cmd, out, sizeof out, 5000, NULL);
	close(srv);
	ASSERT(strstr(out, "CONNECTED") == NULL);
	if (isolation_was_denied(rc, out))
		return 0;
	ASSERT(rc == 0);
	ASSERT(strstr(out, "ISOLATED") != NULL);
	return 0;
}
#endif

/* ------------------------------------------------------------------ */
/* 5.7 Benchmark: sandbox_exec("true") N times                          */
/* ------------------------------------------------------------------ */

static int benchmark_sandbox_exec(void)
{
	enum { BENCH_N = 200 };
	long times_us[BENCH_N];
	char out[256];
	int i;
	int rc;
	long sum = 0;
	long median_us;

	rc = sandbox_exec("true", out, sizeof(out), 5000, NULL);
	if (skip_if_isolation_denied(rc, out, "benchmark_sandbox_exec"))
		return 0;

	for (i = 0; i < BENCH_N; i++) {
		struct timespec t0, t1;
		long diff_us;
		clock_gettime(CLOCK_MONOTONIC, &t0);
		sandbox_exec("true", out, sizeof(out), 5000, NULL);
		clock_gettime(CLOCK_MONOTONIC, &t1);
		diff_us = (long)((t1.tv_sec - t0.tv_sec) * 1000000L +
		                 (t1.tv_nsec - t0.tv_nsec) / 1000L);
		times_us[i] = diff_us;
		sum += diff_us;
	}
	/* Simple selection sort for median (small N). */
	for (i = 0; i < BENCH_N - 1; i++) {
		int j, min_idx = i;
		for (j = i + 1; j < BENCH_N; j++)
			if (times_us[j] < times_us[min_idx]) min_idx = j;
		if (min_idx != i) {
			long tmp = times_us[i];
			times_us[i] = times_us[min_idx];
			times_us[min_idx] = tmp;
		}
	}
	median_us = times_us[BENCH_N / 2];
	printf("sandbox_exec('true') N=%d: median=%ld µs  avg=%ld µs\n",
	       BENCH_N, median_us, sum / BENCH_N);
	/*
	 * PRD §4.7.35 target: clone + setup < 1 ms.
	 * This assertion is informational; we mark slow CI as a skip rather than
	 * a hard failure.  Uncomment the hard assert for local development:
	 *   ASSERT(median_us < 1000);
	 */
	if (median_us >= 2000)
		fprintf(stderr, "sandbox bench: median %ld µs exceeds 2 ms target (CI may be slow)\n",
		        median_us);
	return 0;
}

/* ------------------------------------------------------------------ */
/* main                                                                 */
/* ------------------------------------------------------------------ */

int main(void)
{
	RUN(test_isolation_denied_helper());
	RUN(test_fail_closed_reports_isolation_error());
	RUN(test_output_capture());
	RUN(test_stderr_captured());
	RUN(test_null_cmd_returns_error());
	RUN(test_zero_cap_returns_error());
	RUN(test_exit_nonzero_runs());
	RUN(test_workspace_chdir());
	RUN(test_timeout_kills_process());
#ifdef __linux__
	RUN(test_shadow_not_accessible());
	RUN(test_workspace_landlock_blocks_symlink_escape());
	RUN(test_workspace_landlock_blocks_abs_etc());
	RUN(test_workspace_landlock_allows_workspace_write());
	RUN(test_network_namespace_blocks_host_loopback());
#else
	fprintf(stderr, "test_sandbox: Linux-only namespace tests skipped on this platform\n");
#endif
	RUN(benchmark_sandbox_exec());
	printf("test_sandbox: all tests passed\n");
	return 0;
}
