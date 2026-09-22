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

static int test_block_memory_sidecars_and_bare_names(void)
{
	allowlist_config_t acfg;

	ASSERT(allowlist_path_is_runtime_state_file("/tmp/x/.shellclaw/memory.db-wal") == 1);
	ASSERT(allowlist_path_is_runtime_state_file("/tmp/x/.shellclaw/memory.db-shm") == 1);
	ASSERT(allowlist_path_is_runtime_state_file("/tmp/proj/memory.db-wal") == 0);
	acfg.workspace_path = "/tmp/x/.shellclaw";
	acfg.workspace_only = 1;
	ASSERT(allowlist_check_shell_command("cat config.toml", &acfg, NULL, 0) == 1);
	ASSERT(allowlist_check_shell_command("cat memory.db", &acfg, NULL, 0) == 1);
	ASSERT(allowlist_check_shell_command("cat memory.db-wal", &acfg, NULL, 0) == 1);
	ASSERT(allowlist_check_shell_command("cat notes.txt", &acfg, NULL, 0) == 0);
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

static int test_workspace_only_blocks_quoted_path(void)
{
	allowlist_config_t cfg;
	char reason[256];

	cfg.workspace_path = "/tmp";
	cfg.workspace_only = 1;
	reason[0] = '\0';
	ASSERT(allowlist_check_shell_command("cat '/etc/passwd'", &cfg, reason, sizeof(reason)) == 1);
	ASSERT(allowlist_check_shell_command("cat \"/etc/passwd\"", &cfg, reason, sizeof(reason)) == 1);
	return 0;
}

static int test_workspace_only_blocks_embedded_path_in_python(void)
{
	allowlist_config_t cfg;
	char reason[256];

	cfg.workspace_path = "/tmp";
	cfg.workspace_only = 1;
	reason[0] = '\0';
	ASSERT(allowlist_check_shell_command(
		"python3 -c \"open('/etc/passwd').read()\"", &cfg, reason, sizeof(reason)) == 1);
	ASSERT(strstr(reason, "passwd") != NULL || strstr(reason, "workspace") != NULL);
	return 0;
}

static int test_workspace_only_allows_relative_and_url_slashes(void)
{
	allowlist_config_t cfg;
	char reason[256];

	cfg.workspace_path = "/tmp";
	cfg.workspace_only = 1;
	reason[0] = '\0';
	ASSERT(allowlist_check_shell_command("echo 3/4", &cfg, reason, sizeof(reason)) == 0);
	ASSERT(allowlist_check_shell_command("ls src/foo", &cfg, reason, sizeof(reason)) == 0);
	ASSERT(allowlist_check_shell_command(
		"curl https://example.com/api", &cfg, reason, sizeof(reason)) == 0);
	return 0;
}

static int test_workspace_only_blocks_file_url(void)
{
	allowlist_config_t cfg;
	char reason[256];

	cfg.workspace_path = "/tmp";
	cfg.workspace_only = 1;
	reason[0] = '\0';
	ASSERT(allowlist_check_shell_command("cat file:///etc/passwd", &cfg, reason, sizeof(reason)) == 1);
	return 0;
}

static int test_workspace_only_blocks_home_env_expansion(void)
{
	allowlist_config_t cfg;
	char reason[256];
	char ws[] = "/tmp/sc_al_home_XXXXXX";
	char *dir;

	dir = mkdtemp(ws);
	if (!dir) {
		fprintf(stderr, "test_workspace_only_blocks_home_env_expansion: mkdtemp failed\n");
		return 1;
	}
	cfg.workspace_path = dir;
	cfg.workspace_only = 1;
	reason[0] = '\0';
	ASSERT(allowlist_check_shell_command("cat $HOME/.shellclaw/auth_tokens.json",
	                                    &cfg, reason, sizeof(reason)) == 1);
	ASSERT(strstr(reason, "escapes workspace") != NULL ||
	       strstr(reason, "unresolved") != NULL ||
	       strstr(reason, "forbidden") != NULL);
	reason[0] = '\0';
	ASSERT(allowlist_check_shell_command("cat ${HOME}/.shellclaw/auth_tokens.json",
	                                    &cfg, reason, sizeof(reason)) == 1);
	reason[0] = '\0';
	ASSERT(allowlist_check_shell_command("cat $PWD/../outside.txt",
	                                    &cfg, reason, sizeof(reason)) == 1);
	reason[0] = '\0';
	ASSERT(allowlist_check_shell_command("cat $'\\x2fetc\\x2fpasswd'",
	                                    &cfg, reason, sizeof(reason)) == 1);
	reason[0] = '\0';
	ASSERT(allowlist_check_shell_command("cat \"$HOME/.shellclaw/auth_tokens.json\"",
	                                    &cfg, reason, sizeof(reason)) == 1);
	reason[0] = '\0';
	ASSERT(allowlist_check_shell_command("cat notes.txt", &cfg, reason, sizeof(reason)) == 0);
	rmdir(dir);
	return 0;
}

static int test_workspace_only_blocks_glued_shell_expansions(void)
{
	allowlist_config_t cfg;
	char reason[256];
	char ws[] = "/tmp/sc_al_glue_XXXXXX";
	char *dir;
	char *old_pwd;
	char pwd_copy[256];
	char outside[512];
	int rc;

	dir = mkdtemp(ws);
	if (!dir) {
		fprintf(stderr, "test_workspace_only_blocks_glued_shell_expansions: mkdtemp failed\n");
		return 1;
	}
	cfg.workspace_path = dir;
	cfg.workspace_only = 1;
	reason[0] = '\0';
	ASSERT(allowlist_check_shell_command("cat$IFS/etc/passwd",
	                                    &cfg, reason, sizeof(reason)) == 1);
	reason[0] = '\0';
	ASSERT(allowlist_check_shell_command("cat${IFS}/etc/passwd",
	                                    &cfg, reason, sizeof(reason)) == 1);
	reason[0] = '\0';
	ASSERT(allowlist_check_shell_command("cat$'\\x20/etc/passwd'",
	                                    &cfg, reason, sizeof(reason)) == 1);
	reason[0] = '\0';
	ASSERT(allowlist_check_shell_command(
		"python3 -c \"open('$HOME/.shellclaw/auth_tokens.json')\"",
		&cfg, reason, sizeof(reason)) == 1);
	reason[0] = '\0';
	ASSERT(allowlist_check_shell_command("cat\"$HOME/.bashrc\"",
	                                    &cfg, reason, sizeof(reason)) == 1);
	old_pwd = getenv("PWD");
	pwd_copy[0] = '\0';
	if (old_pwd) {
		if (strlen(old_pwd) >= sizeof(pwd_copy)) {
			rmdir(dir);
			fprintf(stderr, "test_workspace_only_blocks_glued_shell_expansions: PWD too long\n");
			return 1;
		}
		memcpy(pwd_copy, old_pwd, strlen(old_pwd) + 1);
	}
	if (setenv("PWD", dir, 1) != 0) {
		rmdir(dir);
		fprintf(stderr, "test_workspace_only_blocks_glued_shell_expansions: setenv PWD failed\n");
		return 1;
	}
	snprintf(outside, sizeof(outside), "%s/../sc_al_pwd_stolen_%d.txt", dir, (int)getpid());
	reason[0] = '\0';
	rc = allowlist_check_shell_command("cat $PWD/../sc_al_pwd_stolen.txt",
	                                   &cfg, reason, sizeof(reason));
	if (pwd_copy[0])
		(void)setenv("PWD", pwd_copy, 1);
	else
		(void)unsetenv("PWD");
	ASSERT(rc == 1);
	ASSERT(allowlist_path_is_under_workspace(outside, dir) == 0);
	rmdir(dir);
	return 0;
}

static int test_dotdot_escape_nonexistent_destination(void)
{
	char workspace[] = "/tmp/sc_al_ws_XXXXXX";
	char *ws;
	char new_file[256];
	char escape_path[256];
	char cmd[640];
	allowlist_config_t cfg;
	char reason[256];

	ws = mkdtemp(workspace);
	if (!ws) {
		fprintf(stderr, "test_dotdot_escape_nonexistent_destination: mkdtemp failed\n");
		return 1;
	}
	snprintf(new_file, sizeof(new_file), "%s/brand_new.txt", ws);
	ASSERT(allowlist_path_is_under_workspace(new_file, ws) == 1);
	snprintf(escape_path, sizeof(escape_path),
	         "%s/../../tmp/sc_al_stolen_%d", ws, (int)getpid());
	ASSERT(allowlist_path_is_under_workspace(escape_path, ws) == 0);
	cfg.workspace_path = ws;
	cfg.workspace_only = 1;
	reason[0] = '\0';
	snprintf(cmd, sizeof(cmd), "cp %s/memory.db %s", ws, escape_path);
	ASSERT(allowlist_check_shell_command(cmd, &cfg, reason, sizeof(reason)) == 1);
	rmdir(ws);
	return 0;
}

static int test_dotdot_escape_missing_component_before_dotdot(void)
{
	char workspace[] = "/tmp/sc_al_ws_XXXXXX";
	char *ws;
	char escape_path[256];
	char cmd[640];
	allowlist_config_t cfg;
	char reason[256];

	ws = mkdtemp(workspace);
	if (!ws) {
		fprintf(stderr, "test_dotdot_escape_missing_component_before_dotdot: mkdtemp failed\n");
		return 1;
	}
	snprintf(escape_path, sizeof(escape_path),
	         "%s/nope/../../../tmp/sc_al_stolen2_%d", ws, (int)getpid());
	ASSERT(allowlist_path_is_under_workspace(escape_path, ws) == 0);
	cfg.workspace_path = ws;
	cfg.workspace_only = 1;
	reason[0] = '\0';
	snprintf(cmd, sizeof(cmd), "cp %s/memory.db %s", ws, escape_path);
	ASSERT(allowlist_check_shell_command(cmd, &cfg, reason, sizeof(reason)) == 1);
	rmdir(ws);
	return 0;
}

static int test_workspace_only_blocks_file_url_variants(void)
{
	allowlist_config_t cfg;
	char reason[256];

	cfg.workspace_path = "/tmp";
	cfg.workspace_only = 1;
	reason[0] = '\0';
	ASSERT(allowlist_check_shell_command("curl file:/etc/passwd",
	                                    &cfg, reason, sizeof(reason)) == 1);
	reason[0] = '\0';
	ASSERT(allowlist_check_shell_command("curl file://localhost/etc/passwd",
	                                    &cfg, reason, sizeof(reason)) == 1);
	reason[0] = '\0';
	ASSERT(allowlist_check_shell_command("curl file://etc/passwd",
	                                    &cfg, reason, sizeof(reason)) == 1);
	reason[0] = '\0';
	ASSERT(allowlist_check_shell_command("curl https://example.com/api",
	                                    &cfg, reason, sizeof(reason)) == 0);
	return 0;
}

static int test_workspace_only_blocks_embedded_relative_dotdot(void)
{
	allowlist_config_t cfg;
	char reason[256];
	char ws[] = "/tmp/sc_al_rel_XXXXXX";
	char *dir;

	dir = mkdtemp(ws);
	if (!dir) {
		fprintf(stderr, "test_workspace_only_blocks_embedded_relative_dotdot: mkdtemp failed\n");
		return 1;
	}
	cfg.workspace_path = dir;
	cfg.workspace_only = 1;
	reason[0] = '\0';
	ASSERT(allowlist_check_shell_command("python3 -c \"open('../secret')\"",
	                                    &cfg, reason, sizeof(reason)) == 1);
	reason[0] = '\0';
	ASSERT(allowlist_check_shell_command("python3 -c \"open('foo/../../etc/passwd')\"",
	                                    &cfg, reason, sizeof(reason)) == 1);
	reason[0] = '\0';
	ASSERT(allowlist_check_shell_command("python3 -c \"open('notes.txt')\"",
	                                    &cfg, reason, sizeof(reason)) == 0);
	rmdir(dir);
	return 0;
}

static int test_workspace_only_blocks_home_pwd_assignment(void)
{
	allowlist_config_t cfg;
	char reason[256];
	char ws[] = "/tmp/sc_al_asgn_XXXXXX";
	char *dir;

	dir = mkdtemp(ws);
	if (!dir) {
		fprintf(stderr, "test_workspace_only_blocks_home_pwd_assignment: mkdtemp failed\n");
		return 1;
	}
	cfg.workspace_path = dir;
	cfg.workspace_only = 1;
	reason[0] = '\0';
	ASSERT(allowlist_check_shell_command("PWD=; cat $PWD/etc/passwd",
	                                    &cfg, reason, sizeof(reason)) == 1);
	reason[0] = '\0';
	ASSERT(allowlist_check_shell_command("HOME=; cat $HOME/etc/passwd",
	                                    &cfg, reason, sizeof(reason)) == 1);
	reason[0] = '\0';
	ASSERT(allowlist_check_shell_command("export PWD=; cat $PWD/etc/passwd",
	                                    &cfg, reason, sizeof(reason)) == 1);
	reason[0] = '\0';
	ASSERT(allowlist_check_shell_command("unset HOME; cat $HOME/etc/passwd",
	                                    &cfg, reason, sizeof(reason)) == 1);
	rmdir(dir);
	return 0;
}

static int test_workspace_only_blocks_symlink_dotdot(void)
{
#ifdef __linux__
	char workspace[] = "/tmp/sc_al_sydd_XXXXXX";
	char link_path[256];
	char escape_path[512];
	char cmd[640];
	char *ws;
	allowlist_config_t cfg;
	char reason[256];

	ws = mkdtemp(workspace);
	if (!ws) {
		fprintf(stderr, "test_workspace_only_blocks_symlink_dotdot: mkdtemp failed\n");
		return 1;
	}
	snprintf(link_path, sizeof(link_path), "%s/out", ws);
	if (symlink("/", link_path) != 0) {
		rmdir(ws);
		fprintf(stderr, "test_workspace_only_blocks_symlink_dotdot: symlink failed\n");
		return 1;
	}
	snprintf(escape_path, sizeof(escape_path), "%s/../etc/passwd", link_path);
	ASSERT(allowlist_path_is_under_workspace(escape_path, ws) == 0);
	cfg.workspace_path = ws;
	cfg.workspace_only = 1;
	reason[0] = '\0';
	snprintf(cmd, sizeof(cmd), "cat %s/../etc/passwd", link_path);
	ASSERT(allowlist_check_shell_command(cmd, &cfg, reason, sizeof(reason)) == 1);
	unlink(link_path);
	rmdir(ws);
	return 0;
#else
	fprintf(stderr, "test_workspace_only_blocks_symlink_dotdot: skipped (Linux-specific)\n");
	return 0;
#endif
}

static int test_workspace_only_blocks_percent_encoded_file_url(void)
{
	allowlist_config_t cfg;
	char reason[256];

	cfg.workspace_path = "/tmp";
	cfg.workspace_only = 1;
	reason[0] = '\0';
	ASSERT(allowlist_check_shell_command("curl file:///etc/%2e%2e/%2e%2e/etc/passwd",
	                                    &cfg, reason, sizeof(reason)) == 1);
	reason[0] = '\0';
	ASSERT(allowlist_check_shell_command("curl https://example.com/api",
	                                    &cfg, reason, sizeof(reason)) == 0);
	return 0;
}

static int test_relative_symlink_indirection(void)
{
#ifdef __linux__
	char workspace[] = "/tmp/sc_al_reltok_XXXXXX";
	char leak_path[256];
	char *ws;
	allowlist_config_t cfg;
	char reason[256];

	ws = mkdtemp(workspace);
	if (!ws) {
		fprintf(stderr, "test_relative_symlink_indirection: mkdtemp failed\n");
		return 1;
	}
	snprintf(leak_path, sizeof(leak_path), "%s/leak", ws);
	if (symlink("/etc/passwd", leak_path) != 0) {
		rmdir(ws);
		fprintf(stderr, "test_relative_symlink_indirection: symlink failed\n");
		return 1;
	}
	cfg.workspace_path = ws;
	cfg.workspace_only = 1;
	reason[0] = '\0';
	ASSERT(allowlist_check_shell_command("cat leak", &cfg, reason, sizeof(reason)) == 1);
	ASSERT(strstr(reason, "escapes") != NULL || strstr(reason, "workspace") != NULL);
	reason[0] = '\0';
	ASSERT(allowlist_check_shell_command("echo hello", &cfg, reason, sizeof(reason)) == 0);
	unlink(leak_path);
	rmdir(ws);
	return 0;
#else
	fprintf(stderr, "test_relative_symlink_indirection: skipped (Linux-specific)\n");
	return 0;
#endif
}

static int test_runtime_state_edges_and_url_hosts(void)
{
	allowlist_config_t cfg;
	char reason[256];

	ASSERT(allowlist_path_is_runtime_state_file(NULL) == 0);
	ASSERT(allowlist_path_is_runtime_state_file("") == 0);
	ASSERT(allowlist_path_is_runtime_state_file("shellclaw.pid") == 1);
	ASSERT(allowlist_path_is_runtime_state_file("shellclaw.log") == 1);
	ASSERT(allowlist_path_is_runtime_state_file("config.toml") == 0);
	ASSERT(allowlist_path_is_runtime_state_file("/config.toml") == 0);
	ASSERT(allowlist_path_is_runtime_state_file("/.shellclaw/config.toml") == 1);
	ASSERT(allowlist_path_is_runtime_state_file("/.shellclaw/memory.db") == 1);
	ASSERT(allowlist_check_shell_command("cat shellclaw.pid", NULL, NULL, 0) == 1);
	ASSERT(allowlist_check_shell_command("cat shellclaw.log", NULL, NULL, 0) == 1);
	ASSERT(allowlist_check_shell_command("cat config.toml", NULL, NULL, 0) == 0);
	ASSERT(allowlist_path_is_under_workspace(NULL, "/tmp") == 0);
	ASSERT(allowlist_path_is_under_workspace("/tmp/a", NULL) == 0);
	ASSERT(allowlist_path_is_under_workspace("/tmp/a", "") == 0);
	ASSERT(allowlist_path_is_under_workspace("foo/../../etc/passwd", "/tmp") == 0);
	cfg.workspace_path = "/tmp";
	cfg.workspace_only = 1;
	reason[0] = '\0';
	ASSERT(allowlist_check_shell_command("curl file://127.0.0.1/etc/passwd",
	                                    &cfg, reason, sizeof(reason)) == 1);
	reason[0] = '\0';
	ASSERT(allowlist_check_shell_command("curl file:///etc/%2",
	                                    &cfg, reason, sizeof(reason)) == 1);
	reason[0] = '\0';
	ASSERT(allowlist_check_shell_command("curl file:///etc/%GG",
	                                    &cfg, reason, sizeof(reason)) == 1);
	return 0;
}

/* Sandbox-off substring-matches these names. Sandbox-on must too: a
 * python -c open() is not a shell token whose basename is the file. */
static int test_runtime_state_names_block_embedded(void)
{
	allowlist_config_t cfg;

	cfg.workspace_path = "/tmp";
	cfg.workspace_only = 1;
	ASSERT(allowlist_check_shell_command(
		"python3 -c \"print(open('auth_tokens.json').read())\"",
		&cfg, NULL, 0) == 1);
	ASSERT(allowlist_check_shell_command(
		"python3 -c \"open('shellclaw.pid')\"",
		&cfg, NULL, 0) == 1);
	ASSERT(allowlist_check_shell_command(
		"python3 -c \"open('shellclaw.log')\"",
		&cfg, NULL, 0) == 1);
	return 0;
}

/* $HOME/$PWD that stay inside the workspace take the scanner's allow path.
 * A temp workspace makes those expansions fail closed, so these lines were
 * never hit. Also cover token-walker and file-URL edges around that path. */
static int test_in_workspace_env_and_scanner_edges(void)
{
	allowlist_config_t cfg;
	char reason[256];
	char cwd[PATH_MAX];
	char ws[] = "/tmp/sc_al_cov_XXXXXX";
	const char *home;
	const char *pwd;
	char *dir;

	home = getenv("HOME");
	pwd = getenv("PWD");
	if (!home || !home[0] || !getcwd(cwd, sizeof(cwd))) {
		fprintf(stderr, "test_in_workspace_env_and_scanner_edges: HOME or cwd unavailable\n");
		return 1;
	}
	if (!pwd || !pwd[0])
		pwd = cwd;
	cfg.workspace_only = 1;
	cfg.workspace_path = home;
	reason[0] = '\0';
	ASSERT(allowlist_check_shell_command("echo $HOME/notes.txt",
	                                    &cfg, reason, sizeof(reason)) == 0);
	reason[0] = '\0';
	ASSERT(allowlist_check_shell_command("echo ${HOME}/notes.txt",
	                                    &cfg, reason, sizeof(reason)) == 0);
	reason[0] = '\0';
	ASSERT(allowlist_check_shell_command("echo ~/notes.txt",
	                                    &cfg, reason, sizeof(reason)) == 0);
	reason[0] = '\0';
	ASSERT(allowlist_check_shell_command("echo $HOME.extra",
	                                    &cfg, reason, sizeof(reason)) == 1);
	ASSERT(strstr(reason, "unresolved") != NULL);
	cfg.workspace_path = pwd;
	reason[0] = '\0';
	ASSERT(allowlist_check_shell_command("echo $PWD/notes.txt",
	                                    &cfg, reason, sizeof(reason)) == 0);
	reason[0] = '\0';
	ASSERT(allowlist_check_shell_command("echo ${PWD}/notes.txt",
	                                    &cfg, reason, sizeof(reason)) == 0);
	cfg.workspace_path = "/tmp";
	reason[0] = '\0';
	ASSERT(allowlist_check_shell_command("/bin/echo hi",
	                                    &cfg, reason, sizeof(reason)) == 1);
	reason[0] = '\0';
	ASSERT(allowlist_check_shell_command("curl file:///%2E%2E/etc/passwd",
	                                    &cfg, reason, sizeof(reason)) == 1);
	reason[0] = '\0';
	ASSERT(allowlist_check_shell_command("echo xfile:///tmp/inside",
	                                    &cfg, reason, sizeof(reason)) == 0);
	reason[0] = '\0';
	ASSERT(allowlist_check_shell_command("echo file://localhost",
	                                    &cfg, reason, sizeof(reason)) == 0);
	reason[0] = '\0';
	ASSERT(allowlist_check_shell_command("unset FOO; echo notes.txt",
	                                    &cfg, reason, sizeof(reason)) == 0);
	reason[0] = '\0';
	ASSERT(allowlist_check_shell_command("export BAR; echo notes.txt",
	                                    &cfg, reason, sizeof(reason)) == 0);
	reason[0] = '\0';
	ASSERT(allowlist_check_shell_command("echo -1",
	                                    &cfg, reason, sizeof(reason)) == 0);
	reason[0] = '\0';
	ASSERT(allowlist_check_shell_command("python3 -c \"open('..')\"",
	                                    &cfg, reason, sizeof(reason)) == 1);
	reason[0] = '\0';
	ASSERT(allowlist_check_shell_command("echo ..;",
	                                    &cfg, reason, sizeof(reason)) == 1);
	reason[0] = '\0';
	ASSERT(allowlist_check_shell_command("echo https://example.com/.git",
	                                    &cfg, reason, sizeof(reason)) == 0);
	reason[0] = '\0';
	ASSERT(allowlist_check_shell_command("cat ${HOME}/auth_tokens.json",
	                                    NULL, reason, sizeof(reason)) == 1);
	reason[0] = '\0';
	ASSERT(allowlist_check_shell_command("cat $PWD/shellclaw.pid",
	                                    NULL, reason, sizeof(reason)) == 1);
	dir = mkdtemp(ws);
	if (!dir) {
		fprintf(stderr, "test_in_workspace_env_and_scanner_edges: mkdtemp failed\n");
		return 1;
	}
	ASSERT(allowlist_path_is_under_workspace("foo/..", dir) == 0);
	ASSERT(allowlist_path_is_under_workspace("foo/./bar", dir) == 0);
	rmdir(dir);
	return 0;
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
	RUN(test_block_memory_sidecars_and_bare_names());
	RUN(test_allow_project_config_toml());
	RUN(test_runtime_state_names_block_embedded());
	RUN(test_runtime_state_edges_and_url_hosts());
	RUN(test_in_workspace_env_and_scanner_edges());
	RUN(test_path_inside_workspace());
	RUN(test_path_outside_workspace());
	RUN(test_path_prefix_no_slash());
	RUN(test_workspace_only_blocks_outside_path());
	RUN(test_workspace_only_allows_inside_path());
	RUN(test_workspace_only_blocks_quoted_path());
	RUN(test_workspace_only_blocks_embedded_path_in_python());
	RUN(test_workspace_only_allows_relative_and_url_slashes());
	RUN(test_workspace_only_blocks_file_url());
	RUN(test_workspace_only_blocks_home_env_expansion());
	RUN(test_workspace_only_blocks_glued_shell_expansions());
	RUN(test_symlink_escape());
	RUN(test_dotdot_escape_nonexistent_destination());
	RUN(test_dotdot_escape_missing_component_before_dotdot());
	RUN(test_workspace_only_blocks_file_url_variants());
	RUN(test_workspace_only_blocks_embedded_relative_dotdot());
	RUN(test_workspace_only_blocks_home_pwd_assignment());
	RUN(test_workspace_only_blocks_symlink_dotdot());
	RUN(test_workspace_only_blocks_percent_encoded_file_url());
	RUN(test_relative_symlink_indirection());
	printf("test_allowlist: all tests passed\n");
	return 0;
}
