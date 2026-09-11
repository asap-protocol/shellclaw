/**
 * @file shell.c
 * @brief Shell tool: execute commands, with optional sandbox isolation.
 *
 * When config_sandbox_enabled() is true, commands are checked via
 * allowlist_check_shell_command() and executed inside sandbox_exec() (namespace
 * isolation + cgroups v2 where available).
 *
 * When the sandbox is disabled (default), a best-effort substring blocklist
 * is applied and the command runs via fork()/execl() with the same pipe-and-
 * timeout loop used in previous phases.  A warning is printed to stderr so
 * operators are aware of the reduced isolation.
 */
#define _POSIX_C_SOURCE 200809L

#include "tools/tool.h"
#include "tools/shell.h"
#include "core/config.h"
#include "sandbox/sandbox.h"
#include "sandbox/allowlist.h"
#include "cJSON.h"
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_TIMEOUT_SEC 60
#define DEFAULT_OUTPUT_CAP  (256 * 1024)

static const char SHELL_PARAMS[] =
	"{\"type\":\"object\",\"properties\":{\"command\":{\"type\":\"string\",\"description\":\"Shell command to execute\"}},\"required\":[\"command\"]}";

/**
 * Best-effort fallback blocklist used when the full sandbox is disabled.
 * NOT a security boundary — see comments in sandbox/allowlist.c for the real check.
 */
static const char *const FALLBACK_BLOCKLIST[] = {
	"rm -rf /", "rm -rf / ", "rm -rf /$", "rm -rf /*",
	"mkfs", "dd if=", "dd of=", "shutdown", "reboot",
	":(){ :|:& };:", "fork()", "> /dev/sd",
	NULL
};

static int fallback_is_blocked(const char *cmd)
{
	if (!cmd) return 1;
	for (const char *const *p = FALLBACK_BLOCKLIST; *p; p++) {
		if (strstr(cmd, *p) != NULL)
			return 1;
	}
	return 0;
}

static const config_t *g_shell_cfg;

static void kill_command_tree(pid_t pid)
{
	if (kill(-pid, SIGKILL) != 0)
		(void)kill(pid, SIGKILL);
}

/* Kill leftover children after drain (timeout or output cap), matching sandbox_exec. */
static int reap_running_child(pid_t pid)
{
	int status = 0;
	int wr = waitpid(pid, &status, WNOHANG);
	int i;
	struct timespec ts;

	if (wr != 0)
		return 0;
	kill_command_tree(pid);
	for (i = 0; i < 40; i++) {
		wr = waitpid(pid, &status, WNOHANG);
		if (wr != 0)
			return 1;
		ts.tv_sec = 0;
		ts.tv_nsec = 50 * 1000 * 1000;
		(void)nanosleep(&ts, NULL);
	}
	(void)waitpid(pid, &status, 0);
	return 1;
}

static void append_unsandboxed_output(char *result_buf, size_t max_len, size_t *total,
                                      const char *chunk, size_t n)
{
	size_t add = n;

	if (*total + add >= max_len - 1)
		add = max_len - 1 - *total;
	if (add == 0)
		return;
	memcpy(result_buf + *total, chunk, add);
	*total += add;
	result_buf[*total] = '\0';
}

/* ------------------------------------------------------------------ */
/* Unsandboxed execution (fork + poll + waitpid)                        */
/* ------------------------------------------------------------------ */

static int run_unsandboxed(const char *command, int timeout_sec,
                           char *result_buf, size_t max_len)
{
	int pipefd[2];
	pid_t pid;
	size_t total = 0;
	int timed_out = 0;
	int elapsed_ms = 0;
	char buf[256];
	if (pipe(pipefd) != 0) {
		snprintf(result_buf, max_len, "{\"error\":\"pipe failed\"}");
		return -1;
	}
	pid = fork();
	if (pid < 0) {
		close(pipefd[0]);
		close(pipefd[1]);
		snprintf(result_buf, max_len, "{\"error\":\"fork failed\"}");
		return -1;
	}
	if (pid == 0) {
		close(pipefd[0]);
		dup2(pipefd[1], STDOUT_FILENO);
		dup2(pipefd[1], STDERR_FILENO);
		close(pipefd[1]);
		(void)setpgid(0, 0);
		execl("/bin/sh", "sh", "-c", command, (char *)NULL);
		_exit(127);
	}
	(void)setpgid(pid, pid);
	close(pipefd[1]);
	result_buf[0] = '\0';
	while (total < max_len - 1 && elapsed_ms < timeout_sec * 1000) {
		struct pollfd pfd;
		int rem;
		int r;
		ssize_t n;
		pfd.fd = pipefd[0];
		pfd.events = POLLIN;
		pfd.revents = 0;
		rem = timeout_sec * 1000 - elapsed_ms;
		if (rem > 5000) rem = 5000;
		r = poll(&pfd, 1, rem);
		if (r < 0) {
			if (errno == EINTR) continue;
			break;
		}
		if (r == 0) {
			elapsed_ms += rem;
			if (elapsed_ms >= timeout_sec * 1000) {
				timed_out = 1;
				kill_command_tree(pid);
				break;
			}
			continue;
		}
		n = read(pipefd[0], buf, sizeof(buf));
		if (n <= 0) break;
		append_unsandboxed_output(result_buf, max_len, &total, buf, (size_t)n);
	}
	result_buf[total] = '\0';
	close(pipefd[0]);
	if (reap_running_child(pid))
		timed_out = 1;
	if (timed_out && total < max_len - 32)
		snprintf(result_buf + total, max_len - total, "\n[Command timed out]");
	return 0;
}

/* ------------------------------------------------------------------ */
/* Tool execute callback                                                */
/* ------------------------------------------------------------------ */

static int shell_execute(const char *args_json, char *result_buf, size_t max_len)
{
	cJSON *root;
	cJSON *cmd_j;
	char *command;
	int sandbox_on;
	int timeout_sec;
	if (!args_json || !result_buf || max_len == 0) return -1;
	root = cJSON_Parse(args_json);
	if (!root || !cJSON_IsObject(root)) {
		if (root) cJSON_Delete(root);
		snprintf(result_buf, max_len, "{\"error\":\"invalid JSON\"}");
		return -1;
	}
	cmd_j = cJSON_GetObjectItem(root, "command");
	if (!cmd_j || !cJSON_IsString(cmd_j)) {
		cJSON_Delete(root);
		snprintf(result_buf, max_len, "{\"error\":\"missing or invalid 'command'\"}");
		return -1;
	}
	command = strdup(cmd_j->valuestring ? cmd_j->valuestring : "");
	cJSON_Delete(root);
	if (!command) {
		snprintf(result_buf, max_len, "{\"error\":\"out of memory\"}");
		return -1;
	}
	sandbox_on = g_shell_cfg ? config_sandbox_enabled(g_shell_cfg) : 0;
	timeout_sec = g_shell_cfg ? config_shell_timeout_sec(g_shell_cfg) : DEFAULT_TIMEOUT_SEC;
	if (sandbox_on) {
		/* Full sandbox path: allowlist check + sandbox_exec */
		allowlist_config_t acfg;
		sandbox_config_t scfg;
		char reason[256];
		int rc;
		acfg.workspace_path = g_shell_cfg ? config_workspace_path(g_shell_cfg) : NULL;
		acfg.workspace_only = g_shell_cfg ? config_workspace_only(g_shell_cfg) : 0;
		reason[0] = '\0';
		if (allowlist_check_shell_command(command, &acfg, reason, sizeof(reason))) {
			free(command);
			snprintf(result_buf, max_len, "{\"error\":\"command blocked: %s\"}",
			         reason[0] ? reason : "allowlist");
			return -1;
		}
		scfg.workspace_path = acfg.workspace_path;
		scfg.memory_max_bytes = g_shell_cfg
		                        ? config_sandbox_memory_max_bytes(g_shell_cfg) : 0;
		scfg.cpu_max = g_shell_cfg ? config_sandbox_cpu_max(g_shell_cfg) : NULL;
		scfg.cgroup_base = g_shell_cfg ? config_sandbox_cgroup_base(g_shell_cfg) : NULL;
		rc = sandbox_exec(command, result_buf, max_len,
		                  timeout_sec * 1000, &scfg);
		free(command);
		return rc;
	}
	/* Fallback path: best-effort blocklist + plain fork */
	fprintf(stderr,
	        "shell: sandbox disabled — running command with reduced isolation\n");
	if (fallback_is_blocked(command)) {
		free(command);
		snprintf(result_buf, max_len, "{\"error\":\"command blocked for safety\"}");
		return -1;
	}
	{
		int rc = run_unsandboxed(command, timeout_sec, result_buf, max_len);
		free(command);
		return rc;
	}
}

static const tool_t SHELL_TOOL = {
	.name = "shell",
	.description = "Execute a shell command. Returns stdout and stderr. Dangerous commands are blocked.",
	.parameters_json = SHELL_PARAMS,
	.execute = shell_execute,
};

const tool_t *tool_shell_get(void)
{
	return &SHELL_TOOL;
}

void tool_shell_set_config(const config_t *cfg)
{
	g_shell_cfg = cfg;
}
