/**
 * @file allowlist.h
 * @brief Shell-command allowlist: conservative block rules and workspace path checks.
 *
 * allowlist_check_shell_command() should be called before executing any shell command
 * when the sandbox is enabled.  It combines two independent layers of defence:
 *
 *  1. A built-in substring blocklist (dangerous patterns like "rm -rf /",
 *     "mkfs", "dd of=/dev/", fork bombs, etc.).
 *  2. An optional workspace-containment check: if enabled via allowlist_config_t,
 *     path-like tokens in the command are resolved with realpath(3) and rejected
 *     when they escape the declared workspace root.
 *
 * Both checks are intentionally conservative and may produce false positives.
 * They are a best-effort defence-in-depth layer; real isolation is provided by
 * sandbox_exec() via kernel namespaces.
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
     * When non-NULL and workspace_only is non-zero, any path argument that
     * resolves outside this prefix is rejected.
     */
    const char *workspace_path;
    /**
     * When non-zero, any token that looks like a path is subjected to
     * realpath()-based containment checks against workspace_path.
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
 */
int allowlist_check_shell_command(const char *cmd, const allowlist_config_t *cfg,
                                  char *reason_buf, size_t reason_cap);

/**
 * Check whether @p path is contained inside @p workspace_root after resolving symlinks.
 *
 * Uses realpath(3) when the path exists. If it does not, walks to the first
 * existing ancestor and checks that resolved directory (so `..` cannot escape
 * by targeting a file that has not been created yet).
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
