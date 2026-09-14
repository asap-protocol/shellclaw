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
 *     when they escape the declared workspace root. Quoted and embedded absolute
 *     paths (`cat '/etc/passwd'`, `python3 -c "open('/etc/passwd')"`) are scanned
 *     on the full command because whitespace tokenization misses them. `$HOME`,
 *     `${HOME}`, `$PWD`, `${PWD}`, and other `$...` forms are also scanned on the
 *     full command so glued expansions (`cat$IFS/etc/passwd`, `cat"$HOME/.bashrc"`,
 *     ANSI-C `$'\x20/...'`) cannot skip tok[0]. Known HOME/PWD forms are expanded
 *     (including a following `/...` suffix); other `$` forms fail closed. `file:`
 *     URLs are extracted even when `://` hides the path slash, then percent-decoded
 *     so `%2e%2e` / `%2f` cannot hide an escape. Missing directories before `..`
 *     are collapsed lexically so `/ws/nope/../../../tmp` cannot stop at `/ws`;
 *     `..` is not cancelled across a symlink. Embedded relative `../` is joined
 *     to the workspace before the same check. Encoded leading slashes (`\\x2f`,
 *     `\\57`, `\\u002f`, `\\u{2f}`, `\\x{2f}`, `\\o{57}`) are reconstructed as `/` or `../` plus the
 *     following path body. `\\N{` fail-closes without parsing Unicode names.
 *     In-command `HOME=` / `PWD=` / `export` / `unset` of those names fail closed
 *     even inside quotes (`eval 'PWD=;'`) or after a comma; `env -i`, `env -iu`,
 *     `env -u` / `--unset` HOME|PWD, POSIX `read HOME|PWD`, and `os.environ.pop`/`del`/
 *     `clear` / `os.unsetenv` / `os.putenv` of those names fail closed. Quote-split `file:` schemes
 *     (`f'ile://...`, `'f'+'ile://...'`) and hex/unicode/octal-hidden schemes
 *     (`\\x66ile:`, `\\u0066ile:`, `\\146ile:`, `f\\ile:`) are joined before the URL check.
 *     POSIX `\\` + newline line continuation is collapsed before HOME/PWD and `file:` scans.
 *     `printf -v HOME|PWD` and `os.environ["HOME"]=` / `.update({"HOME":...})` fail closed.
 *     The same hex/unicode/octal/identity decode used for `file:` recovery runs before
 *     the HOME/PWD keyword gate and the `$` scan (`PW\\D=`, `\\unset`, `\\x24HOME`,
 *     `\\044`, `\\u0024`). `declare -n` targeting HOME|PWD and `exec -c` fail closed.
 *     `../` after `://` is still containment-checked so URL-disguised walks cannot skip the gate.
 *
 * Both checks are intentionally conservative and may produce false positives.
 * They are a defence-in-depth layer. The kernel host-FS bound for the shell
 * tool is Landlock in sandbox_exec() when a workspace path is set; namespaces
 * fail closed if they cannot apply. workspace_only path scanning does not
 * replace that bound (and is not a language interpreter: `chr(47)+` stays
 * residual on this scanner).
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
