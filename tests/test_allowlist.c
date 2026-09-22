/**
 * @file test_allowlist.c
 * @brief Unit tests for allowlist_check_shell_command and allowlist_path_is_under_workspace.
 *
 * Tests cover: built-in blocklist patterns, workspace-only path containment,
 * realpath-based symlink escape detection, and edge cases (NULL, empty string).
 */
#if defined(__APPLE__)
#define _DARWIN_C_SOURCE
#endif
#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "sandbox/allowlist.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define ASSERT(c) do { \
	if (!(c)) { \
		fprintf(stderr, "FAIL: %s:%d  %s\n", __FILE__, __LINE__, #c); \
		return 1; \
	} \
} while (0)

#define RUN(t) do { int r_ = (t); if (r_) return r_; } while (0)

/* ------------------------------------------------------------------ */
/* Built-in blocklist                                                   */
/* ------------------------------------------------------------------ */

static int test_block_rm_rf_root(void)
{
	char reason[256];
	ASSERT(allowlist_check_shell_command("rm -rf /", NULL, reason, sizeof(reason)) == 1);
	ASSERT(strstr(reason, "blocked") != NULL || strstr(reason, "forbidden") != NULL);
	return 0;
}

static int test_block_rm_rf_wildcard(void)
{
	ASSERT(allowlist_check_shell_command("rm -rf /*", NULL, NULL, 0) == 1);
	return 0;
}

static int test_block_mkfs(void)
{
	ASSERT(allowlist_check_shell_command("mkfs.ext4 /dev/sda", NULL, NULL, 0) == 1);
	return 0;
}

static int test_block_dd_devsd(void)
{
	ASSERT(allowlist_check_shell_command("dd if=/dev/zero of=/dev/sda", NULL, NULL, 0) == 1);
	return 0;
}

static int test_block_fork_bomb(void)
{
	ASSERT(allowlist_check_shell_command(":(){ :|:& };:", NULL, NULL, 0) == 1);
	return 0;
}

static int test_block_shutdown(void)
{
	ASSERT(allowlist_check_shell_command("shutdown -h now", NULL, NULL, 0) == 1);
	return 0;
}

static int test_block_etc_shadow(void)
{
	ASSERT(allowlist_check_shell_command("cat /etc/shadow", NULL, NULL, 0) == 1);
	return 0;
}

/** Wave 7.3: Argus socket must not be touched from sandboxed shell commands. */
static int test_block_argus_socket(void)
{
	ASSERT(allowlist_check_shell_command("cat /tmp/argus_socket", NULL, NULL, 0) == 1);
	ASSERT(allowlist_check_shell_command("ls -la /tmp/argus_socket", NULL, NULL, 0) == 1);
	return 0;
}

/** Wave 7.1: Jetson GPU device paths must not be opened from sandboxed shell. */
static int test_block_jetson_gpu_devices(void)
{
	ASSERT(allowlist_check_shell_command("cat /dev/nvgpu", NULL, NULL, 0) == 1);
	ASSERT(allowlist_check_shell_command("dd if=/dev/nvmap", NULL, NULL, 0) == 1);
	ASSERT(allowlist_check_shell_command("ls /dev/nvhost-ctrl", NULL, NULL, 0) == 1);
	return 0;
}

static int test_allow_safe_command(void)
{
	ASSERT(allowlist_check_shell_command("ls -la /tmp", NULL, NULL, 0) == 0);
	return 0;
}

static int test_allow_echo(void)
{
	ASSERT(allowlist_check_shell_command("echo hello", NULL, NULL, 0) == 0);
	return 0;
}

static int test_null_command_blocked(void)
{
	ASSERT(allowlist_check_shell_command(NULL, NULL, NULL, 0) == 1);
	return 0;
}

static int test_block_auth_tokens_json(void)
{
	char reason[256];
	ASSERT(allowlist_check_shell_command("cat ~/.shellclaw/auth_tokens.json",
	                                    NULL, reason, sizeof(reason)) == 1);
	ASSERT(allowlist_path_is_runtime_state_file("auth_tokens.json") == 1);
	return 0;
}

static int test_block_state_dir_config_and_memory(void)
{
	char dir[] = "/tmp/sc_al_state_XXXXXX";
	char state[PATH_MAX - 32];
	char cfg_path[PATH_MAX];
	char db_path[PATH_MAX];
	char *tmp;
	FILE *f;
	allowlist_config_t acfg;
	char cmd[PATH_MAX + 16];

	tmp = mkdtemp(dir);
	if (!tmp) {
		fprintf(stderr, "test_block_state_dir_config_and_memory: mkdtemp failed\n");
		return 1;
	}
	snprintf(state, sizeof(state), "%s/.shellclaw", tmp);
	if (mkdir(state, 0755) != 0) {
		rmdir(tmp);
		return 1;
	}
	snprintf(cfg_path, sizeof(cfg_path), "%s/config.toml", state);
	snprintf(db_path, sizeof(db_path), "%s/memory.db", state);
	f = fopen(cfg_path, "w");
	if (!f) {
		rmdir(state);
		rmdir(tmp);
		return 1;
	}
	fputs("x=1\n", f);
	fclose(f);
	f = fopen(db_path, "w");
	if (!f) {
		unlink(cfg_path);
		rmdir(state);
		rmdir(tmp);
		return 1;
	}
	fputs("db", f);
	fclose(f);
	ASSERT(allowlist_path_is_runtime_state_file(cfg_path) == 1);
	ASSERT(allowlist_path_is_runtime_state_file(db_path) == 1);
	acfg.workspace_path = state;
	acfg.workspace_only = 1;
	snprintf(cmd, sizeof(cmd), "cat %s", cfg_path);
	ASSERT(allowlist_check_shell_command(cmd, &acfg, NULL, 0) == 1);
	snprintf(cmd, sizeof(cmd), "cat %s", db_path);
	ASSERT(allowlist_check_shell_command(cmd, &acfg, NULL, 0) == 1);
	unlink(cfg_path);
	unlink(db_path);
	rmdir(state);
	rmdir(tmp);
	return 0;
}

static int test_allow_project_config_toml(void)
{
	ASSERT(allowlist_path_is_runtime_state_file("/tmp/project/config.toml") == 0);
	ASSERT(allowlist_path_is_runtime_state_file("/tmp/project/memory.db") == 0);
	return 0;
}

/* ------------------------------------------------------------------ */
/* Workspace path containment                                           */
/* ------------------------------------------------------------------ */

static int test_path_inside_workspace(void)
{
	ASSERT(allowlist_path_is_under_workspace("/tmp", "/tmp") == 1);
	ASSERT(allowlist_path_is_under_workspace("/tmp/foo", "/tmp") == 1);
	ASSERT(allowlist_path_is_under_workspace("/tmp/foo/bar", "/tmp") == 1);
	return 0;
}

static int test_path_outside_workspace(void)
{
	ASSERT(allowlist_path_is_under_workspace("/etc/passwd", "/tmp") == 0);
	ASSERT(allowlist_path_is_under_workspace("/home/user", "/tmp") == 0);
	return 0;
}

static int test_path_prefix_no_slash(void)
{
	/* /tmpfoo should NOT match /tmp as workspace root */
	ASSERT(allowlist_path_is_under_workspace("/tmpfoo", "/tmp") == 0);
	return 0;
}

/* ------------------------------------------------------------------ */
/* Workspace-only blocking via config                                   */
/* ------------------------------------------------------------------ */

static int test_workspace_only_blocks_outside_path(void)
{
	allowlist_config_t cfg;
	char reason[256];
	cfg.workspace_path = "/tmp/sc_ws_test";
	cfg.workspace_only = 1;
	reason[0] = '\0';
	ASSERT(allowlist_check_shell_command("cat /etc/passwd", &cfg, reason, sizeof(reason)) == 1);
	return 0;
}

static int test_workspace_only_allows_inside_path(void)
{
	allowlist_config_t cfg;
	char reason[256];
	char cmd[256];
	/* Use /tmp as workspace; the command only touches /tmp paths. */
	cfg.workspace_path = "/tmp";
	cfg.workspace_only = 1;
	reason[0] = '\0';
	snprintf(cmd, sizeof(cmd), "ls /tmp");
	ASSERT(allowlist_check_shell_command(cmd, &cfg, reason, sizeof(reason)) == 0);
	return 0;
}

/* ------------------------------------------------------------------ */
/* Symlink escape test (5.4)                                            */
/* ------------------------------------------------------------------ */

static int test_symlink_escape(void)
{
#ifdef __linux__
	char workspace[] = "/tmp/sc_al_ws_XXXXXX";
	char link_path[256];
	char *ws;
	allowlist_config_t cfg;
	char reason[256];
	char cmd[512];
	ws = mkdtemp(workspace);
	if (!ws) {
		fprintf(stderr, "test_symlink_escape: mkdtemp failed, skipping\n");
		return 0;
	}
	snprintf(link_path, sizeof(link_path), "%s/secret_link", ws);
	/* Create a symlink inside workspace pointing to /etc */
	if (symlink("/etc", link_path) != 0) {
		rmdir(ws);
		fprintf(stderr, "test_symlink_escape: symlink failed, skipping\n");
		return 0;
	}
	cfg.workspace_path = ws;
	cfg.workspace_only = 1;
	reason[0] = '\0';
	/* The symlink resolves to /etc which is outside the workspace */
	snprintf(cmd, sizeof(cmd), "cat %s/passwd", link_path);
	ASSERT(allowlist_check_shell_command(cmd, &cfg, reason, sizeof(reason)) == 1);
	unlink(link_path);
	rmdir(ws);
	return 0;
#else
	fprintf(stderr, "test_symlink_escape: skipped (Linux-specific)\n");
	return 0;
#endif
}

/* ------------------------------------------------------------------ */
/* main                                                                 */
/* ------------------------------------------------------------------ */

int main(void)
{
	RUN(test_block_rm_rf_root());
	RUN(test_block_rm_rf_wildcard());
	RUN(test_block_mkfs());
	RUN(test_block_dd_devsd());
	RUN(test_block_fork_bomb());
	RUN(test_block_shutdown());
	RUN(test_block_etc_shadow());
	RUN(test_block_jetson_gpu_devices());
	RUN(test_block_argus_socket());
	RUN(test_allow_safe_command());
	RUN(test_allow_echo());
	RUN(test_null_command_blocked());
	RUN(test_block_auth_tokens_json());
	RUN(test_block_state_dir_config_and_memory());
	RUN(test_allow_project_config_toml());
	RUN(test_path_inside_workspace());
	RUN(test_path_outside_workspace());
	RUN(test_path_prefix_no_slash());
	RUN(test_workspace_only_blocks_outside_path());
	RUN(test_workspace_only_allows_inside_path());
	RUN(test_symlink_escape());
	printf("test_allowlist: all tests passed\n");
	return 0;
}
