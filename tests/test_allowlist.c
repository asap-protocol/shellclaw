/**
 * @file test_allowlist.c
 * @brief Unit tests for allowlist_check_shell_command and allowlist_path_is_under_workspace.
 *
 * Tests cover: built-in blocklist patterns, workspace-only path containment,
 * realpath-based symlink escape detection, and edge cases (NULL, empty string).
 */
#define _POSIX_C_SOURCE 200809L

#include "sandbox/allowlist.h"
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

/**
 * Shell expands `$HOME` / `${HOME}` / `$PWD` before open(2). Tokens never start
 * with `/` `~` `.`, so the old has_path_chars gate skipped them.
 */
static int test_workspace_only_blocks_home_env_expansion(void)
{
	allowlist_config_t cfg;
	char reason[256];
	char ws[] = "/tmp/sc_al_home_XXXXXX";
	char *dir;
	const char *home = getenv("HOME");

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
	       strstr(reason, "unresolved") != NULL);
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
	if (home && strcmp(home, dir) == 0) {
		reason[0] = '\0';
		ASSERT(allowlist_check_shell_command("ls $HOME", &cfg, reason, sizeof(reason)) == 0);
	}
	rmdir(dir);
	return 0;
}

/**
 * Glued expansions never start a strtok token with `$` or `/`, so the host-FS
 * gate must scan `$` on the full command. `$PWD/../outside` must use the
 * process PWD, not only a mkdtemp workspace that happens to differ from PWD.
 */
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

	{
		const char *old_home = getenv("HOME");
		char home_copy[256];

		home_copy[0] = '\0';
		if (old_home) {
			if (strlen(old_home) >= sizeof(home_copy)) {
				rmdir(dir);
				fprintf(stderr, "test_workspace_only_blocks_glued_shell_expansions: HOME too long\n");
				return 1;
			}
			memcpy(home_copy, old_home, strlen(old_home) + 1);
		}
		if (setenv("HOME", dir, 1) != 0) {
			rmdir(dir);
			fprintf(stderr, "test_workspace_only_blocks_glued_shell_expansions: setenv HOME failed\n");
			return 1;
		}
		reason[0] = '\0';
		rc = allowlist_check_shell_command("ls ${HOME}", &cfg, reason, sizeof(reason));
		if (rc == 0)
			rc = allowlist_check_shell_command("ls $HOME", &cfg, reason, sizeof(reason));
		if (home_copy[0])
			(void)setenv("HOME", home_copy, 1);
		else
			(void)unsetenv("HOME");
		ASSERT(rc == 0);
	}

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
/* Non-existent path with .. must not escape via lexical prefix         */
/* ------------------------------------------------------------------ */

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

	/* realpath() fails for a new file; the workspace ancestor must still allow it. */
	snprintf(new_file, sizeof(new_file), "%s/brand_new.txt", ws);
	ASSERT(allowlist_path_is_under_workspace(new_file, ws) == 1);

	/*
	 * Destination does not exist, so realpath() fails. A lexical prefix check
	 * treats workspace/../../tmp/... as inside the workspace.
	 */
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

/**
 * A missing directory *before* `..` must not stop the ancestor walk at the
 * workspace. `/ws/nope/../../../tmp/stolen` lexically leaves `/ws`.
 */
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
	ASSERT(allowlist_check_shell_command("python3 -c \"urllib.request.urlopen('file://localhost/etc/passwd')\"",
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

/**
 * Interpreter hex/octal/unicode slash escapes decode to `/` before a path
 * body. A later literal `/` (`etc/passwd`) is not a path start, so the
 * host-FS gate must reconstruct the encoded leading slash. This is not
 * Python `chr(47)+` concatenation (no slash encoding in the command text).
 */
static int test_workspace_only_blocks_encoded_leading_slash(void)
{
	allowlist_config_t cfg;
	char reason[256];

	cfg.workspace_path = "/tmp";
	cfg.workspace_only = 1;
	reason[0] = '\0';
	ASSERT(allowlist_check_shell_command(
		"python3 -c \"open('\\x2fetc/passwd')\"", &cfg, reason, sizeof(reason)) == 1);
	reason[0] = '\0';
	ASSERT(allowlist_check_shell_command(
		"node -e \"require('fs').readFileSync('\\x2fetc/passwd')\"",
		&cfg, reason, sizeof(reason)) == 1);
	reason[0] = '\0';
	ASSERT(allowlist_check_shell_command(
		"python3 -c \"open('\\57etc/passwd')\"", &cfg, reason, sizeof(reason)) == 1);
	reason[0] = '\0';
	ASSERT(allowlist_check_shell_command(
		"node -e \"require('fs').readFileSync('\\u002fetc/passwd')\"",
		&cfg, reason, sizeof(reason)) == 1);
	reason[0] = '\0';
	ASSERT(allowlist_check_shell_command(
		"python3 -c \"open('notes.txt')\"", &cfg, reason, sizeof(reason)) == 0);
	return 0;
}

/**
 * `$PWD` / `$HOME` expansion must not trust process getenv when the command
 * assigns, exports, or unsets those names. Process PWD/HOME are set to the
 * workspace so a getenv-only check would incorrectly allow `$PWD/etc/passwd`.
 */
static int test_workspace_only_blocks_home_pwd_assignment(void)
{
	allowlist_config_t cfg;
	char reason[256];
	char ws[] = "/tmp/sc_al_asgn_XXXXXX";
	char *dir;
	const char *old_pwd;
	const char *old_home;
	char pwd_copy[256];
	char home_copy[256];

	dir = mkdtemp(ws);
	if (!dir) {
		fprintf(stderr, "test_workspace_only_blocks_home_pwd_assignment: mkdtemp failed\n");
		return 1;
	}
	cfg.workspace_path = dir;
	cfg.workspace_only = 1;

	old_pwd = getenv("PWD");
	pwd_copy[0] = '\0';
	if (old_pwd) {
		if (strlen(old_pwd) >= sizeof(pwd_copy)) {
			rmdir(dir);
			fprintf(stderr, "test_workspace_only_blocks_home_pwd_assignment: PWD too long\n");
			return 1;
		}
		memcpy(pwd_copy, old_pwd, strlen(old_pwd) + 1);
	}
	old_home = getenv("HOME");
	home_copy[0] = '\0';
	if (old_home) {
		if (strlen(old_home) >= sizeof(home_copy)) {
			rmdir(dir);
			fprintf(stderr, "test_workspace_only_blocks_home_pwd_assignment: HOME too long\n");
			return 1;
		}
		memcpy(home_copy, old_home, strlen(old_home) + 1);
	}
	if (setenv("PWD", dir, 1) != 0 || setenv("HOME", dir, 1) != 0) {
		rmdir(dir);
		fprintf(stderr, "test_workspace_only_blocks_home_pwd_assignment: setenv failed\n");
		return 1;
	}

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

	if (pwd_copy[0])
		(void)setenv("PWD", pwd_copy, 1);
	else
		(void)unsetenv("PWD");
	if (home_copy[0])
		(void)setenv("HOME", home_copy, 1);
	else
		(void)unsetenv("HOME");
	rmdir(dir);
	return 0;
}

/**
 * Kernel open(2) walks a symlink before `..`. Lexical collapse must not
 * treat `workspace/out/../etc/passwd` as `workspace/etc/passwd` when `out`
 * is a directory symlink to `/`.
 */
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

/**
 * `file:` URLs percent-decode before the workspace check. Encoded `..`
 * (`%2e%2e`) and `%2f` must not hide an escape. `https://` stays allowed.
 */
static int test_workspace_only_blocks_percent_encoded_file_url(void)
{
	allowlist_config_t cfg;
	char reason[256];
	char ws[] = "/tmp/sc_al_pct_XXXXXX";
	char *dir;
	char cmd[768];

	dir = mkdtemp(ws);
	if (!dir) {
		fprintf(stderr, "test_workspace_only_blocks_percent_encoded_file_url: mkdtemp failed\n");
		return 1;
	}
	cfg.workspace_path = dir;
	cfg.workspace_only = 1;
	reason[0] = '\0';
	snprintf(cmd, sizeof(cmd),
	         "curl file://%s/%%2e%%2e/%%2e%%2e/%%2e%%2e/etc/passwd", dir);
	ASSERT(allowlist_check_shell_command(cmd, &cfg, reason, sizeof(reason)) == 1);
	reason[0] = '\0';
	ASSERT(allowlist_check_shell_command("curl file://localhost/%2fetc/passwd",
	                                    &cfg, reason, sizeof(reason)) == 1);
	reason[0] = '\0';
	ASSERT(allowlist_check_shell_command("curl https://example.com/api",
	                                    &cfg, reason, sizeof(reason)) == 0);
	rmdir(dir);
	return 0;
}

/**
 * Quote immediately before `PWD=` / nested `eval` / `sh -c` must still
 * fail closed. Process PWD/HOME are the workspace so getenv-only expansion
 * would allow `$PWD/etc/passwd`.
 */
static int test_workspace_only_blocks_quoted_home_pwd_assignment(void)
{
	allowlist_config_t cfg;
	char reason[256];
	char ws[] = "/tmp/sc_al_qasgn_XXXXXX";
	char *dir;
	const char *old_pwd;
	const char *old_home;
	char pwd_copy[256];
	char home_copy[256];

	dir = mkdtemp(ws);
	if (!dir) {
		fprintf(stderr, "test_workspace_only_blocks_quoted_home_pwd_assignment: mkdtemp failed\n");
		return 1;
	}
	cfg.workspace_path = dir;
	cfg.workspace_only = 1;

	old_pwd = getenv("PWD");
	pwd_copy[0] = '\0';
	if (old_pwd) {
		if (strlen(old_pwd) >= sizeof(pwd_copy)) {
			rmdir(dir);
			fprintf(stderr, "test_workspace_only_blocks_quoted_home_pwd_assignment: PWD too long\n");
			return 1;
		}
		memcpy(pwd_copy, old_pwd, strlen(old_pwd) + 1);
	}
	old_home = getenv("HOME");
	home_copy[0] = '\0';
	if (old_home) {
		if (strlen(old_home) >= sizeof(home_copy)) {
			rmdir(dir);
			fprintf(stderr, "test_workspace_only_blocks_quoted_home_pwd_assignment: HOME too long\n");
			return 1;
		}
		memcpy(home_copy, old_home, strlen(old_home) + 1);
	}
	if (setenv("PWD", dir, 1) != 0 || setenv("HOME", dir, 1) != 0) {
		rmdir(dir);
		fprintf(stderr, "test_workspace_only_blocks_quoted_home_pwd_assignment: setenv failed\n");
		return 1;
	}

	reason[0] = '\0';
	ASSERT(allowlist_check_shell_command("eval 'PWD=; cat $PWD/etc/passwd'",
	                                    &cfg, reason, sizeof(reason)) == 1);
	reason[0] = '\0';
	ASSERT(allowlist_check_shell_command("sh -c 'PWD=; cat $PWD/etc/passwd'",
	                                    &cfg, reason, sizeof(reason)) == 1);
	reason[0] = '\0';
	ASSERT(allowlist_check_shell_command("eval 'HOME=; cat $HOME/etc/passwd'",
	                                    &cfg, reason, sizeof(reason)) == 1);
	reason[0] = '\0';
	ASSERT(allowlist_check_shell_command("sh -c \"unset HOME; cat $HOME/etc/passwd\"",
	                                    &cfg, reason, sizeof(reason)) == 1);

	if (pwd_copy[0])
		(void)setenv("PWD", pwd_copy, 1);
	else
		(void)unsetenv("PWD");
	if (home_copy[0])
		(void)setenv("HOME", home_copy, 1);
	else
		(void)unsetenv("HOME");
	rmdir(dir);
	return 0;
}

/**
 * Quotes (and trivial quote-concat) must not split the `file:` scheme.
 * `https://` stays allowed.
 */
static int test_workspace_only_blocks_quote_split_file_url(void)
{
	allowlist_config_t cfg;
	char reason[256];

	cfg.workspace_path = "/tmp";
	cfg.workspace_only = 1;
	reason[0] = '\0';
	ASSERT(allowlist_check_shell_command("curl f'ile://localhost/etc/passwd'",
	                                    &cfg, reason, sizeof(reason)) == 1);
	reason[0] = '\0';
	ASSERT(allowlist_check_shell_command("curl f\"ile:/etc/passwd\"",
	                                    &cfg, reason, sizeof(reason)) == 1);
	reason[0] = '\0';
	ASSERT(allowlist_check_shell_command(
		"python3 -c \"urllib.request.urlopen('f'+'ile://localhost/etc/passwd')\"",
		&cfg, reason, sizeof(reason)) == 1);
	reason[0] = '\0';
	ASSERT(allowlist_check_shell_command("curl https://example.com/api",
	                                    &cfg, reason, sizeof(reason)) == 0);
	return 0;
}

/**
 * Encoded `.` (`\x2e` / `\56` / `\u002e`) forms `../`, and extra slash
 * encodings (`\u{2f}`, `\N{SOLIDUS}`) decode to `/`. `\N{` fail-closes
 * without parsing Unicode names. Not `chr(47)+` concatenation.
 */
static int test_workspace_only_blocks_encoded_dot_and_named_slash(void)
{
	allowlist_config_t cfg;
	char reason[256];
	char ws[] = "/tmp/sc_al_edot_XXXXXX";
	char *dir;

	dir = mkdtemp(ws);
	if (!dir) {
		fprintf(stderr, "test_workspace_only_blocks_encoded_dot_and_named_slash: mkdtemp failed\n");
		return 1;
	}
	cfg.workspace_path = dir;
	cfg.workspace_only = 1;
	reason[0] = '\0';
	ASSERT(allowlist_check_shell_command(
		"python3 -c \"open('\\x2e\\x2e/secret')\"", &cfg, reason, sizeof(reason)) == 1);
	reason[0] = '\0';
	ASSERT(allowlist_check_shell_command(
		"python3 -c \"open('\\56\\56/secret')\"", &cfg, reason, sizeof(reason)) == 1);
	reason[0] = '\0';
	ASSERT(allowlist_check_shell_command(
		"node -e \"require('fs').readFileSync('\\u{2f}etc/passwd')\"",
		&cfg, reason, sizeof(reason)) == 1);
	reason[0] = '\0';
	ASSERT(allowlist_check_shell_command(
		"python3 -c \"open('\\N{SOLIDUS}etc/passwd')\"",
		&cfg, reason, sizeof(reason)) == 1);
	reason[0] = '\0';
	ASSERT(allowlist_check_shell_command(
		"python3 -c \"open('notes.txt')\"", &cfg, reason, sizeof(reason)) == 0);
	rmdir(dir);
	return 0;
}

/**
 * `is_inside_url` must not hide `../` after `://`. Real https fetches without
 * a `..` walk stay allowed.
 */
static int test_workspace_only_blocks_url_disguised_dotdot(void)
{
	allowlist_config_t cfg;
	char reason[256];
	char ws[] = "/tmp/sc_al_url_XXXXXX";
	char *dir;

	dir = mkdtemp(ws);
	if (!dir) {
		fprintf(stderr, "test_workspace_only_blocks_url_disguised_dotdot: mkdtemp failed\n");
		return 1;
	}
	cfg.workspace_path = dir;
	cfg.workspace_only = 1;
	reason[0] = '\0';
	ASSERT(allowlist_check_shell_command(
		"curl https://example.com/../../../../etc/passwd",
		&cfg, reason, sizeof(reason)) == 1);
	reason[0] = '\0';
	ASSERT(allowlist_check_shell_command(
		"cat https://example.com/../../../../etc/passwd",
		&cfg, reason, sizeof(reason)) == 1);
	reason[0] = '\0';
	ASSERT(allowlist_check_shell_command("curl https://example.com/api",
	                                    &cfg, reason, sizeof(reason)) == 0);
	rmdir(dir);
	return 0;
}

/**
 * Perl braced hex `\x{2f}` / `\x{2e}` must reconstruct like `\u{2f}`.
 * Not `chr(47)+` concatenation.
 */
static int test_workspace_only_blocks_perl_braced_hex(void)
{
	allowlist_config_t cfg;
	char reason[256];
	char ws[] = "/tmp/sc_al_perl_XXXXXX";
	char *dir;

	dir = mkdtemp(ws);
	if (!dir) {
		fprintf(stderr, "test_workspace_only_blocks_perl_braced_hex: mkdtemp failed\n");
		return 1;
	}
	cfg.workspace_path = dir;
	cfg.workspace_only = 1;
	reason[0] = '\0';
	ASSERT(allowlist_check_shell_command(
		"perl -e 'open F, \"\\x{2f}etc/passwd\"'",
		&cfg, reason, sizeof(reason)) == 1);
	reason[0] = '\0';
	ASSERT(allowlist_check_shell_command(
		"perl -e 'open F, \"\\x{2e}\\x{2e}/secret\"'",
		&cfg, reason, sizeof(reason)) == 1);
	reason[0] = '\0';
	ASSERT(allowlist_check_shell_command(
		"perl -e 'open F, \"notes.txt\"'",
		&cfg, reason, sizeof(reason)) == 0);
	rmdir(dir);
	return 0;
}

/**
 * HOME/PWD mutation after comma (argv lists) and env replacement without
 * `HOME=` (`env -i`, `env -u HOME`, `os.environ.pop`) must fail closed.
 */
static int test_workspace_only_blocks_env_replace_and_comma_assign(void)
{
	allowlist_config_t cfg;
	char reason[256];
	char ws[] = "/tmp/sc_al_envr_XXXXXX";
	char *dir;
	const char *old_pwd;
	const char *old_home;
	char pwd_copy[256];
	char home_copy[256];

	dir = mkdtemp(ws);
	if (!dir) {
		fprintf(stderr, "test_workspace_only_blocks_env_replace_and_comma_assign: mkdtemp failed\n");
		return 1;
	}
	cfg.workspace_path = dir;
	cfg.workspace_only = 1;

	old_pwd = getenv("PWD");
	pwd_copy[0] = '\0';
	if (old_pwd) {
		if (strlen(old_pwd) >= sizeof(pwd_copy)) {
			rmdir(dir);
			fprintf(stderr, "test_workspace_only_blocks_env_replace_and_comma_assign: PWD too long\n");
			return 1;
		}
		memcpy(pwd_copy, old_pwd, strlen(old_pwd) + 1);
	}
	old_home = getenv("HOME");
	home_copy[0] = '\0';
	if (old_home) {
		if (strlen(old_home) >= sizeof(home_copy)) {
			rmdir(dir);
			fprintf(stderr, "test_workspace_only_blocks_env_replace_and_comma_assign: HOME too long\n");
			return 1;
		}
		memcpy(home_copy, old_home, strlen(old_home) + 1);
	}
	if (setenv("PWD", dir, 1) != 0 || setenv("HOME", dir, 1) != 0) {
		rmdir(dir);
		fprintf(stderr, "test_workspace_only_blocks_env_replace_and_comma_assign: setenv failed\n");
		return 1;
	}

	reason[0] = '\0';
	ASSERT(allowlist_check_shell_command("env -i cat $PWD/etc/passwd",
	                                    &cfg, reason, sizeof(reason)) == 1);
	reason[0] = '\0';
	ASSERT(allowlist_check_shell_command("env -u HOME cat $HOME/etc/passwd",
	                                    &cfg, reason, sizeof(reason)) == 1);
	reason[0] = '\0';
	ASSERT(allowlist_check_shell_command(
		"python3 -c \"os.environ.pop('HOME'); open('$HOME/etc/passwd')\"",
		&cfg, reason, sizeof(reason)) == 1);
	reason[0] = '\0';
	ASSERT(allowlist_check_shell_command(
		"python3 -c \"f(a,HOME=''); open('$HOME/etc/passwd')\"",
		&cfg, reason, sizeof(reason)) == 1);

	if (pwd_copy[0])
		(void)setenv("PWD", pwd_copy, 1);
	else
		(void)unsetenv("PWD");
	if (home_copy[0])
		(void)setenv("HOME", home_copy, 1);
	else
		(void)unsetenv("HOME");
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
	RUN(test_workspace_only_blocks_encoded_leading_slash());
	RUN(test_workspace_only_blocks_home_pwd_assignment());
	RUN(test_workspace_only_blocks_symlink_dotdot());
	RUN(test_workspace_only_blocks_percent_encoded_file_url());
	RUN(test_workspace_only_blocks_quoted_home_pwd_assignment());
	RUN(test_workspace_only_blocks_quote_split_file_url());
	RUN(test_workspace_only_blocks_encoded_dot_and_named_slash());
	RUN(test_workspace_only_blocks_url_disguised_dotdot());
	RUN(test_workspace_only_blocks_perl_braced_hex());
	RUN(test_workspace_only_blocks_env_replace_and_comma_assign());
	printf("test_allowlist: all tests passed\n");
	return 0;
}
