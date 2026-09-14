/**
 * @file allowlist.h
 * @brief Shell-command allowlist: blocklist plus workspace path defense-in-depth.
 *
 * allowlist_check_shell_command() runs before sandbox_exec() when the sandbox
 * is enabled. It is not the host-FS bound: Linux Landlock in sandbox_exec()
 * is, when a workspace path is set. This scanner still closes the holes that
 * whitespace tokenization missed:
 *
 *  1. Built-in substring blocklist (rm -rf /, mkfs, Jetson GPU /dev, Argus).
 *  2. workspace_only containment: existing-ancestor walk, lexical `..`
 *     collapse (no cancel across a symlink), quoted/embedded `/` `~` `../`,
 *     `file:` URLs (including percent-encoding), `$HOME`/`$PWD` on the full
 *     command (including glued `$IFS`), fail-closed other `$` forms, in-command
 *     HOME/PWD assignment, and bare relative names (`cat leak`).
 *
 * Conservative false positives (`awk '/foo/'`, `echo HOME=foo`) are accepted.
 * Interpreter concat with no path character (`chr(47)+`) is residual here;
 * Landlock denies the host inode. Encoded-slash cat-and-mouse is frozen.
 */
#ifndef SHELLCLAW_ALLOWLIST_H
#define SHELLCLAW_ALLOWLIST_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Configuration for the allowlist checker. */
typedef struct allowlist_config {
    /**
     * Absolute (or ~-prefixed) path to the workspace root.
     * When non-NULL and workspace_only is non-zero, path arguments that
     * resolve outside this prefix are rejected.
     */
    const char *workspace_path;
    /**
     * When non-zero, path-like tokens and embedded path fragments are
     * subjected to containment checks against workspace_path.
     */
    int workspace_only;
} allowlist_config_t;

/**
 * Check @p cmd against the built-in blocklist and optional workspace constraint.
 *
 * Writes a human-readable reason to @p reason_buf (if non-NULL and @p reason_cap > 0)
 * when the command is blocked.
 *
 * @param cmd        The shell command string to inspect. NULL is treated as blocked.
 * @param cfg        Optional allowlist configuration. NULL = blocklist only, no path check.
 * @param reason_buf Optional buffer for a blocking reason message.
 * @param reason_cap Capacity of @p reason_buf.
 * @return           0 if the command is allowed, 1 if blocked.
 *
 * Example: allowlist_check_shell_command("cat '/etc/passwd'", &cfg, reason, sizeof reason)
 * returns 1 when cfg.workspace_only is set to a workspace other than `/etc`.
 */
int allowlist_check_shell_command(const char *cmd, const allowlist_config_t *cfg,
                                  char *reason_buf, size_t reason_cap);

/**
 * Check whether @p path is contained inside @p workspace_root after resolving symlinks.
 *
 * Uses realpath(3) when the path exists (kernel symlink walk, including `..`
 * after a symlink). If that fails, `..` / `.` are collapsed lexically without
 * cancelling `..` across a symlink, so a missing directory before `..` cannot
 * pin the walk at the workspace. If the collapsed path still does not exist,
 * walks to the first existing ancestor and checks that resolved directory.
 *
 * Example: allowlist_path_is_under_workspace("/ws/../../tmp/x", "/ws") is 0
 * even when /tmp/x does not exist.
 *
 * @param path           Absolute or relative path to test.
 * @param workspace_root Absolute path to the workspace root (already resolved).
 * @return               1 if the path is safely inside the workspace, 0 otherwise.
 */
int allowlist_path_is_under_workspace(const char *path, const char *workspace_root);

#ifdef __cplusplus
}
#endif

#endif /* SHELLCLAW_ALLOWLIST_H */
