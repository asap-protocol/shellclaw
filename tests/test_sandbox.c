/**
 * @file test_sandbox.c
 * @brief Unit tests for sandbox_exec.
 *
 * Linux-specific namespace, Landlock, and cgroup tests are guarded by
 * #ifdef __linux__. GitHub-hosted runners often cannot apply user namespaces
 * (sandbox_exec fail-closes; success-path tests skip). The Landlock
 * builder is exercised in-process via prepare(); restrict_self runs in a
 * child so gcov can still write. Do not weaken sandbox.c for CI.
 *
 * 5.7 Benchmark: run sandbox_exec("true") 200 times and report median.
 * The benchmark is informational only — it does not gate the test suite.
 */
#define _DEFAULT_SOURCE
#define _DARWIN_C_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "sandbox/sandbox.h"
#include "sandbox/sandbox_landlock.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/stat.h>
#include <arpa/inet.h>
#include <dirent.h>
#include <netinet/in.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#endif

#define ASSERT(c) do { \
	if (!(c)) { \
		fprintf(stderr, "FAIL: %s:%d  %s\n", __FILE__, __LINE__, #c); \
		return 1; \
	} \
} while (0)

#define RUN(t) do { int r_ = (t); if (r_) return r_; } while (0)

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

#ifdef __linux__
static int proc_cmdline_has(const char *needle)
{
	DIR *d;
	struct dirent *e;
	d = opendir("/proc");
	if (!d)
		return 0;
	while ((e = readdir(d)) != NULL) {
		char path[288];
		char buf[256];
		FILE *f;
		size_t n;
		size_t i;
		if (e->d_name[0] < '1' || e->d_name[0] > '9')
			continue;
		if (strlen(e->d_name) > 16)
			continue;
		snprintf(path, sizeof path, "/proc/%s/cmdline", e->d_name);
		f = fopen(path, "r");
		if (!f)
			continue;
		n = fread(buf, 1, sizeof buf - 1, f);
		fclose(f);
		for (i = 0; i < n; i++) {
			if (buf[i] == '\0')
				buf[i] = ' ';
		}
		buf[n] = '\0';
		if (strstr(buf, needle) != NULL) {
			closedir(d);
			return 1;
		}
	}
	closedir(d);
	return 0;
}
#endif

static int test_isolation_denied_helper(void)
{
	ASSERT(isolation_was_denied(0, "sandbox: namespace isolation failed") == 0);
	ASSERT(isolation_was_denied(-1, "hello") == 0);
	ASSERT(isolation_was_denied(-1, NULL) == 0);
	ASSERT(isolation_was_denied(-1, "sandbox: namespace isolation failed") == 1);
	ASSERT(isolation_was_denied(-1, "sandbox: Landlock filesystem bound failed") == 1);
	return 0;
}

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
	int rc = sandbox_exec("exit 1", out, sizeof(out), 5000, NULL);
	if (skip_if_isolation_denied(rc, out, "test_exit_nonzero_runs"))
		return 0;
	ASSERT(rc == 0);
	return 0;
}

static int test_exit_122_is_not_isolation_failure(void)
{
	char out[4096];
	int rc = sandbox_exec("exit 122", out, sizeof(out), 5000, NULL);
	if (skip_if_isolation_denied(rc, out, "test_exit_122_is_not_isolation_failure"))
		return 0;
	ASSERT(rc == 0);
	ASSERT(strstr(out, "namespace isolation failed") == NULL);
	ASSERT(strstr(out, "Landlock filesystem bound failed") == NULL);
	return 0;
}

static int test_exit_123_with_workspace_is_not_isolation_failure(void)
{
	char out[4096];
	sandbox_config_t cfg;
	char ws[] = "/tmp/sc_sb_ex_XXXXXX";
	char *dir;
	int rc;

	dir = mkdtemp(ws);
	if (!dir) {
		fprintf(stderr, "test_exit_123_with_workspace_is_not_isolation_failure: mkdtemp failed\n");
		return 1;
	}
	memset(&cfg, 0, sizeof cfg);
	cfg.workspace_path = dir;
	rc = sandbox_exec("exit 123", out, sizeof(out), 5000, &cfg);
	rmdir(dir);
	if (skip_if_isolation_denied(rc, out, "test_exit_123_with_workspace_is_not_isolation_failure"))
		return 0;
	ASSERT(rc == 0);
	ASSERT(strstr(out, "namespace isolation failed") == NULL);
	ASSERT(strstr(out, "Landlock filesystem bound failed") == NULL);
	return 0;
}

static int test_pidns_fork_allows_second_command(void)
{
	char out[4096];
	int rc = sandbox_exec("/bin/echo A; /bin/echo B", out, sizeof(out), 5000, NULL);
	if (skip_if_isolation_denied(rc, out, "test_pidns_fork_allows_second_command"))
		return 0;
	ASSERT(rc == 0);
	ASSERT(strstr(out, "Cannot fork") == NULL);
	ASSERT(strstr(out, "A") != NULL);
	ASSERT(strstr(out, "B") != NULL);
	return 0;
}

static int test_dev_null_is_writable(void)
{
	char out[4096];
	sandbox_config_t cfg;
	char ws[] = "/tmp/sc_sb_null_XXXXXX";
	char *dir;
	int rc;

	dir = mkdtemp(ws);
	if (!dir) {
		fprintf(stderr, "test_dev_null_is_writable: mkdtemp failed\n");
		return 1;
	}
	memset(&cfg, 0, sizeof cfg);
	cfg.workspace_path = dir;
	rc = sandbox_exec("echo hi >/dev/null && echo OK", out, sizeof(out), 5000, &cfg);
	rmdir(dir);
	if (skip_if_isolation_denied(rc, out, "test_dev_null_is_writable"))
		return 0;
	ASSERT(rc == 0);
	ASSERT(strstr(out, "OK") != NULL);
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

static int test_missing_workspace_fail_closed(void)
{
	char out[4096];
	sandbox_config_t cfg;
	int rc;

	memset(&cfg, 0, sizeof cfg);
	memset(out, 0, sizeof out);
	cfg.workspace_path = "/no/such/sc_ws_missing_dir";
	rc = sandbox_exec("echo should_not_run", out, sizeof out, 3000, &cfg);
	ASSERT(rc == -1);
	ASSERT(strstr(out, "should_not_run") == NULL);
	ASSERT(strstr(out, "sandbox:") != NULL);
	return 0;
}

static int test_timeout_kills_process(void)
{
	char out[4096];
	int rc = sandbox_exec("sleep 86401", out, sizeof(out), 300, NULL);
	if (skip_if_isolation_denied(rc, out, "test_timeout_kills_process"))
		return 0;
	ASSERT(rc == 0);
	ASSERT(strstr(out, "timed out") != NULL || strlen(out) == 0);
#ifdef __linux__
	{
		int tries;
		for (tries = 0; tries < 20; tries++) {
			if (!proc_cmdline_has("sleep 86401"))
				break;
			{
				struct timespec ts;
				ts.tv_sec = 0;
				ts.tv_nsec = 50 * 1000 * 1000;
				nanosleep(&ts, NULL);
			}
		}
		ASSERT(proc_cmdline_has("sleep 86401") == 0);
	}
#endif
	return 0;
}

#ifdef __linux__
static int test_inherited_fd_is_closed(void)
{
	char dir[] = "/tmp/sc_sb_fd_XXXXXX";
	char path[256];
	char cmd[128];
	char out[4096];
	char *ws;
	int fd;
	int rc;
	ws = mkdtemp(dir);
	if (!ws) {
		fprintf(stderr, "test_inherited_fd_is_closed: mkdtemp failed\n");
		return 1;
	}
	snprintf(path, sizeof path, "%s/secret", ws);
	fd = open(path, O_CREAT | O_RDWR | O_CLOEXEC, 0600);
	if (fd < 0) {
		rmdir(ws);
		return 1;
	}
	if (write(fd, "SECRET_FD_LEAK\n", 15) != 15) {
		close(fd);
		unlink(path);
		rmdir(ws);
		return 1;
	}
	close(fd);
	fd = open(path, O_RDONLY);
	if (fd < 0) {
		unlink(path);
		rmdir(ws);
		return 1;
	}
	snprintf(cmd, sizeof cmd, "cat /dev/fd/%d 2>&1; echo EXIT:$?", fd);
	rc = sandbox_exec(cmd, out, sizeof out, 5000, NULL);
	close(fd);
	unlink(path);
	rmdir(ws);
	if (skip_if_isolation_denied(rc, out, "test_inherited_fd_is_closed"))
		return 0;
	ASSERT(rc == 0);
	ASSERT(strstr(out, "SECRET_FD_LEAK") == NULL);
	return 0;
}

static int test_proc_is_namespaced(void)
{
	char out[4096];
	int rc;
	rc = sandbox_exec("tr '\\0' ' ' < /proc/1/cmdline; echo", out, sizeof out, 5000, NULL);
	if (skip_if_isolation_denied(rc, out, "test_proc_is_namespaced"))
		return 0;
	ASSERT(rc == 0);
	ASSERT(strstr(out, "systemd") == NULL);
	ASSERT(strstr(out, "sh") != NULL);
	return 0;
}

static int test_landlock_probe_without_restrict(void)
{
	char workspace[] = "/tmp/sc_ll_prep_XXXXXX";
	char *ws;
	ASSERT(sandbox_landlock_restrict_to_workspace(NULL) == 0);
	ASSERT(sandbox_landlock_restrict_to_workspace("") == 0);
	ASSERT(sandbox_landlock_restrict_to_workspace("/no/such/sc_ll_ws") == -1);
	ASSERT(sandbox_landlock_prepare(NULL) == 0);
	ASSERT(sandbox_landlock_prepare("") == 0);
	ASSERT(sandbox_landlock_prepare("/no/such/sc_ll_ws") == -1);
	ws = mkdtemp(workspace);
	if (!ws) {
		fprintf(stderr, "test_landlock_probe_without_restrict: mkdtemp failed\n");
		return 1;
	}
	ASSERT(sandbox_landlock_prepare(ws) == 0);
	rmdir(ws);
	return 0;
}

static int test_landlock_restrict_denies_etc_passwd(void)
{
	char workspace[] = "/tmp/sc_ll_XXXXXX";
	char *ws;
	pid_t pid;
	int st;
	int status;
	ws = mkdtemp(workspace);
	if (!ws) {
		fprintf(stderr, "test_landlock_restrict_denies_etc_passwd: mkdtemp failed\n");
		return 1;
	}
	pid = fork();
	if (pid < 0) {
		rmdir(ws);
		return 1;
	}
	if (pid == 0) {
		FILE *f;
		int pfd;
		if (chdir(ws) != 0)
			_exit(5);
		if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0)
			_exit(3);
		if (sandbox_landlock_restrict_to_workspace(ws) != 0)
			_exit(2);
		f = fopen("ok", "w");
		if (!f)
			_exit(4);
		fclose(f);
		unlink("ok");
		pfd = open("/etc/passwd", O_RDONLY | O_CLOEXEC);
		if (pfd >= 0) {
			close(pfd);
			_exit(1);
		}
		_exit(0);
	}
	if (waitpid(pid, &st, 0) < 0) {
		rmdir(ws);
		return 1;
	}
	rmdir(ws);
	if (!WIFEXITED(st)) {
		fprintf(stderr, "FAIL: tests/test_sandbox.c: landlock child did not exit (st=%d)\n",
			st);
		return 1;
	}
	status = WEXITSTATUS(st);
	if (status == 2 || status == 3) {
		fprintf(stderr, "test_sandbox: skip test_landlock_restrict_denies_etc_passwd (WEXITSTATUS=%d)\n",
			status);
		return 0;
	}
	if (status != 0) {
		fprintf(stderr, "FAIL: tests/test_sandbox.c: test_landlock_restrict_denies_etc_passwd WEXITSTATUS=%d (0=denied 1=passwd_open 4=ws_write 5=chdir)\n",
			status);
		return 1;
	}
	return 0;
}

static int test_landlock_denies_etc_ssl_outside_certs(void)
{
	char workspace[] = "/tmp/sc_ll_ssl_XXXXXX";
	char *ws;
	pid_t pid;
	int st;
	int status;
	int host_cnf;
	int host_private;

	host_cnf = access("/etc/ssl/openssl.cnf", R_OK) == 0;
	host_private = access("/etc/ssl/private", R_OK) == 0;
	if (!host_cnf && !host_private) {
		fprintf(stderr,
			"test_sandbox: skip test_landlock_denies_etc_ssl_outside_certs (no readable /etc/ssl targets)\n");
		return 0;
	}
	ws = mkdtemp(workspace);
	if (!ws) {
		fprintf(stderr, "test_landlock_denies_etc_ssl_outside_certs: mkdtemp failed\n");
		return 1;
	}
	pid = fork();
	if (pid < 0) {
		rmdir(ws);
		return 1;
	}
	if (pid == 0) {
		int pfd;
		int saw_cnf = access("/etc/ssl/openssl.cnf", R_OK) == 0;
		int saw_private = access("/etc/ssl/private", R_OK) == 0;
		if (chdir(ws) != 0)
			_exit(5);
		if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0)
			_exit(3);
		if (sandbox_landlock_restrict_to_workspace(ws) != 0)
			_exit(2);
		if (saw_cnf) {
			pfd = open("/etc/ssl/openssl.cnf", O_RDONLY | O_CLOEXEC);
			if (pfd >= 0) {
				close(pfd);
				_exit(1);
			}
		}
		if (saw_private) {
			pfd = open("/etc/ssl/private", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
			if (pfd >= 0) {
				close(pfd);
				_exit(7);
			}
		}
		_exit(0);
	}
	if (waitpid(pid, &st, 0) < 0) {
		rmdir(ws);
		return 1;
	}
	rmdir(ws);
	if (!WIFEXITED(st)) {
		fprintf(stderr,
			"FAIL: tests/test_sandbox.c: ssl landlock child did not exit (st=%d)\n", st);
		return 1;
	}
	status = WEXITSTATUS(st);
	if (status == 2 || status == 3) {
		fprintf(stderr,
			"test_sandbox: skip test_landlock_denies_etc_ssl_outside_certs (WEXITSTATUS=%d)\n",
			status);
		return 0;
	}
	if (status != 0) {
		fprintf(stderr,
			"FAIL: tests/test_sandbox.c: test_landlock_denies_etc_ssl_outside_certs WEXITSTATUS=%d (0=denied 1=cnf_open 7=private_open)\n",
			status);
		return 1;
	}
	return 0;
}
#endif

#ifdef __linux__
static int test_shadow_not_accessible(void)
{
	char out[4096];
	int rc;

	rc = sandbox_exec("cat /etc/shadow 2>&1 || echo BLOCKED", out, sizeof(out), 5000, NULL);
	if (isolation_was_denied(rc, out))
		return 0;
	ASSERT(rc == 0);
	ASSERT(strlen(out) > 0);
	ASSERT(strstr(out, "root:") == NULL);
	return 0;
}

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
	rc = sandbox_exec("cat /etc/passwd 2>&1; echo EXIT:$?", out, sizeof(out), 5000, &cfg);
	ASSERT(strstr(out, "root:x:") == NULL);
	if (isolation_was_denied(rc, out)) {
		rmdir(ws);
		return 0;
	}
	ASSERT(rc == 0);
	ASSERT(strstr(out, "EXIT:0") == NULL);
	if (access("/usr/bin/python3", X_OK) == 0 || access("/bin/python3", X_OK) == 0) {
		FILE *wrote;
		char buf[64];
		size_t nread;
		rc = sandbox_exec(
			"python3 -c 'open(\"out\",\"w\").write(open(chr(47)+\"etc\"+chr(47)+\"passwd\").read())' 2>&1; "
			"echo EXIT:$?",
			out, sizeof(out), 8000, &cfg);
		snprintf(outp, sizeof(outp), "%s/out", ws);
		wrote = fopen(outp, "r");
		if (wrote) {
			nread = fread(buf, 1, sizeof buf - 1, wrote);
			fclose(wrote);
			buf[nread] = '\0';
			ASSERT(strstr(buf, "root:") == NULL);
		}
		unlink(outp);
		ASSERT(strstr(out, "root:x:") == NULL);
		if (!isolation_was_denied(rc, out)) {
			ASSERT(rc == 0);
			ASSERT(strstr(out, "EXIT:0") == NULL);
		}
	}
	rmdir(ws);
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
	if (fgets(buf, sizeof(buf), f) == NULL) {
		fclose(f);
		unlink(wrote);
		rmdir(ws);
		fprintf(stderr, "FAIL: %s:%d  fgets wrote.txt\n", __FILE__, __LINE__);
		return 1;
	}
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
	if (median_us >= 2000)
		fprintf(stderr, "sandbox bench: median %ld µs exceeds 2 ms target (CI may be slow)\n",
		        median_us);
	return 0;
}

#ifdef __linux__
static int test_command_inherits_cgroup(void)
{
	char probe[160];
	char out[4096];
	char *cg;
	char *dir;
	char *nl;
	int rc;

	snprintf(probe, sizeof probe, "/sys/fs/cgroup/shellclaw_probe_%d", (int)getpid());
	if (mkdir(probe, 0755) != 0) {
		fprintf(stderr,
			"test_sandbox: skip test_command_inherits_cgroup (mkdir errno %d)\n",
			errno);
		return 0;
	}
	rmdir(probe);
	rc = sandbox_exec(
		"echo CG:$(cat /proc/self/cgroup); echo DIR:$(ls /sys/fs/cgroup 2>/dev/null | grep shellclaw_sb_ || true)",
		out, sizeof out, 5000, NULL);
	if (skip_if_isolation_denied(rc, out, "test_command_inherits_cgroup"))
		return 0;
	ASSERT(rc == 0);
	dir = strstr(out, "DIR:");
	if (!dir || dir[4] == '\0' || dir[4] == '\n') {
		fprintf(stderr, "test_sandbox: skip test_command_inherits_cgroup (no cgroup dir)\n");
		return 0;
	}
	cg = strstr(out, "CG:");
	ASSERT(cg != NULL);
	nl = strchr(cg, '\n');
	if (nl)
		*nl = '\0';
	ASSERT(strstr(cg, "shellclaw_sb_") != NULL);
	return 0;
}
#endif

int main(void)
{
	RUN(test_isolation_denied_helper());
	RUN(test_fail_closed_reports_isolation_error());
	RUN(test_output_capture());
	RUN(test_stderr_captured());
	RUN(test_null_cmd_returns_error());
	RUN(test_zero_cap_returns_error());
	RUN(test_exit_nonzero_runs());
	RUN(test_exit_122_is_not_isolation_failure());
	RUN(test_exit_123_with_workspace_is_not_isolation_failure());
	RUN(test_pidns_fork_allows_second_command());
	RUN(test_dev_null_is_writable());
	RUN(test_workspace_chdir());
	RUN(test_missing_workspace_fail_closed());
	RUN(test_timeout_kills_process());
#ifdef __linux__
	RUN(test_landlock_probe_without_restrict());
	RUN(test_command_inherits_cgroup());
	RUN(test_inherited_fd_is_closed());
	RUN(test_proc_is_namespaced());
	RUN(test_shadow_not_accessible());
	RUN(test_workspace_landlock_blocks_symlink_escape());
	RUN(test_workspace_landlock_blocks_abs_etc());
	RUN(test_workspace_landlock_allows_workspace_write());
	RUN(test_network_namespace_blocks_host_loopback());
#else
	fprintf(stderr, "test_sandbox: Linux-only namespace tests skipped on this platform\n");
#endif
	RUN(benchmark_sandbox_exec());
#ifdef __linux__
	RUN(test_landlock_restrict_denies_etc_passwd());
	RUN(test_landlock_denies_etc_ssl_outside_certs());
#endif
	printf("test_sandbox: all tests passed\n");
	return 0;
}
