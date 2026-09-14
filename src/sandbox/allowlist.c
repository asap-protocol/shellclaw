/**
 * @file allowlist.c
 * @brief Shell-command allowlist: built-in blocklist + workspace realpath checks.
 */
#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "sandbox/allowlist.h"
#include <ctype.h>
#include <libgen.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Built-in blocklist patterns                                          */
/* ------------------------------------------------------------------ */

static const char *const BLOCK_SUBSTRINGS[] = {
    /* Filesystem destroyers */
    "rm -rf /",
    "rm -rf /*",
    "rm -rf / ",
    "rm -fr /",
    "mkfs",
    "fdisk",
    "parted",
    "> /dev/sd",
    "dd if=",
    "dd of=/dev",
    /* System lifecycle */
    "shutdown",
    "reboot",
    "halt",
    "poweroff",
    "init 0",
    "init 6",
    "systemctl poweroff",
    "systemctl reboot",
    "systemctl halt",
    /* Fork bombs */
    ":(){ :|:& };:",
    "fork()",
    ":(){:|:&};:",
    /* Privilege escalation */
    "chmod 777 /",
    "chmod -R 777 /",
    "chown root",
    "sudo rm -rf",
    /* Credential / secret file access */
    "/etc/shadow",
    "/etc/gshadow",
    "~/.ssh/id_",
    "id_rsa",
    "id_ed25519",
    /* Jetson Tegra GPU device nodes (audit 7.1 — not bind-mounted, block direct open) */
    "/dev/nvhost",
    "/dev/nvgpu",
    "/dev/nvmap",
    /* Jetson Argus camera daemon socket (audit 7.3 — agent-only, not shell sandbox) */
    "/tmp/argus_socket",
    NULL
};

/* ------------------------------------------------------------------ */
/* Helpers                                                              */
/* ------------------------------------------------------------------ */

static void set_reason(char *buf, size_t cap, const char *prefix, const char *detail)
{
    if (!buf || cap == 0) return;
    if (detail && detail[0])
        snprintf(buf, cap, "%s%s", prefix, detail);
    else
        snprintf(buf, cap, "%s", prefix);
    buf[cap - 1] = '\0';
}

/** Return 1 if @p tok begins with a path-like character. */
static int has_path_chars(const char *tok)
{
    if (!tok || !tok[0]) return 0;
    if (tok[0] == '/' || tok[0] == '~' || tok[0] == '.') return 1;
    if (tok[0] == '$') return 1;
    return 0;
}

static char *strip_surrounding_quotes(char *tok)
{
    size_t n;

    if (!tok || !tok[0]) return tok;
    n = strlen(tok);
    if (n >= 2 && ((tok[0] == '\'' && tok[n - 1] == '\'') ||
                   (tok[0] == '"' && tok[n - 1] == '"'))) {
        tok[n - 1] = '\0';
        return tok + 1;
    }
    return tok;
}

static int expand_env_prefix(const char *tok, const char *prefix, size_t prefix_len,
                              int require_slash_or_end, const char *value,
                              char *expanded, size_t expanded_cap)
{
    const char *suffix;
    int n;

    if (strncmp(tok, prefix, prefix_len) != 0)
        return 1;
    suffix = tok + prefix_len;
    if (require_slash_or_end && suffix[0] != '\0' && suffix[0] != '/')
        return 1;
    if (!value)
        return -1;
    n = snprintf(expanded, expanded_cap, "%s%s", value, suffix);
    if (n < 0 || (size_t)n >= expanded_cap)
        return -1;
    return 0;
}

/**
 * Expand `~`, `$HOME` / `${HOME}`, or `$PWD` / `${PWD}`. Other `$...` forms
 * (ANSI-C, command substitution, unknown vars) fail closed.
 */
static int expand_shell_path_token(const char *tok, char *expanded, size_t expanded_cap)
{
    const char *home;
    const char *cwd;
    int rc;

    if (!tok || !expanded || expanded_cap == 0) return -1;
    if (tok[0] == '~') {
        int n;

        home = getenv("HOME");
        if (home)
            n = snprintf(expanded, expanded_cap, "%s%s", home, tok + 1);
        else
            n = snprintf(expanded, expanded_cap, "%s", tok);
        return (n < 0 || (size_t)n >= expanded_cap) ? -1 : 0;
    }
    if (tok[0] != '$') {
        if (strlen(tok) >= expanded_cap) return -1;
        memcpy(expanded, tok, strlen(tok) + 1);
        return 0;
    }
    if (tok[1] == '\'' || tok[1] == '"' || tok[1] == '(')
        return -1;
    home = getenv("HOME");
    cwd = getenv("PWD");
    rc = expand_env_prefix(tok, "${HOME}", 7, 0, home, expanded, expanded_cap);
    if (rc != 1) return rc;
    rc = expand_env_prefix(tok, "$HOME", 5, 1, home, expanded, expanded_cap);
    if (rc != 1) return rc;
    rc = expand_env_prefix(tok, "${PWD}", 6, 0, cwd, expanded, expanded_cap);
    if (rc != 1) return rc;
    rc = expand_env_prefix(tok, "$PWD", 4, 1, cwd, expanded, expanded_cap);
    if (rc != 1) return rc;
    return -1;
}

static int is_path_body_char(unsigned char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '/' || c == '.' || c == '_' ||
           c == '-' || c == '+' || c == '%' || c == '@';
}

static int is_fs_absolute_path_start(const char *text, const char *p)
{
    unsigned char prev;

    if (!text || !p || (*p != '/' && *p != '~'))
        return 0;
    if (p == text)
        return 1;
    prev = (unsigned char)p[-1];
    if (*p == '/' && prev == ':')
        return 0;
    if (*p == '/' && p >= text + 2 && p[-1] == '/' && p[-2] == ':')
        return 0;
    if (is_path_body_char(prev) && prev != '/')
        return 0;
    /* `${HOME}/x` is one expansion; the slash after `}` is not a new FS root. */
    if (prev == '}')
        return 0;
    return 1;
}

static int expand_tilde_fragment(const char *fragment, char *dest, size_t dest_cap)
{
    const char *home;
    int n;

    if (!fragment || !dest || dest_cap == 0)
        return -1;
    if (fragment[0] != '~') {
        if (strlen(fragment) >= dest_cap)
            return -1;
        memcpy(dest, fragment, strlen(fragment) + 1);
        return 0;
    }
    home = getenv("HOME");
    if (!home)
        home = "";
    n = snprintf(dest, dest_cap, "%s%s", home, fragment + 1);
    if (n < 0 || (size_t)n >= dest_cap)
        return -1;
    return 0;
}

static int block_if_embedded_paths_escape(const char *text, const char *workspace_root,
                                          char *reason_buf, size_t reason_cap)
{
    const char *p;

    if (!text || !workspace_root) return 0;
    for (p = text; *p; p++) {
        char fragment[PATH_MAX];
        char expanded[PATH_MAX];
        size_t n = 0;
        const char *start;

        if (!is_fs_absolute_path_start(text, p))
            continue;
        start = p;
        fragment[n++] = *p++;
        while (*p && is_path_body_char((unsigned char)*p) && n + 1 < sizeof(fragment))
            fragment[n++] = *p++;
        fragment[n] = '\0';
        if (expand_tilde_fragment(fragment, expanded, sizeof(expanded)) != 0) {
            set_reason(reason_buf, reason_cap,
                       "command blocked: path escapes workspace: ", fragment);
            return 1;
        }
        if (!allowlist_path_is_under_workspace(expanded, workspace_root)) {
            set_reason(reason_buf, reason_cap,
                       "command blocked: path escapes workspace: ", expanded);
            fprintf(stderr, "allowlist: blocked path outside workspace: %s\n", expanded);
            return 1;
        }
        if (p > start)
            p--;
    }
    return 0;
}

static int resolved_is_under_workspace(const char *resolved, const char *actual_ws, size_t wlen)
{
    if (!resolved || !actual_ws || wlen == 0) return 0;
    if (strncmp(resolved, actual_ws, wlen) != 0) return 0;
    return resolved[wlen] == '\0' || resolved[wlen] == '/';
}

/*
 * realpath(3) cannot canonicalize a path that does not exist. Walking to the
 * first existing ancestor (same approach as tools/file.c) still collapses `..`
 * through existing directories, so workspace/../../tmp/newfile is denied.
 * A lexical prefix check would allow that destination.
 */
static int existing_ancestor_is_under_workspace(const char *path, const char *actual_ws, size_t wlen)
{
    char path_copy[PATH_MAX];
    char resolved[PATH_MAX];
    int hops;

    if (!path || path[0] == '\0' || strlen(path) >= PATH_MAX) return 0;
    snprintf(path_copy, sizeof(path_copy), "%s", path);
    for (hops = 0; hops < PATH_MAX; hops++) {
        char *dir = dirname(path_copy);

        if (!dir || dir[0] == '\0') return 0;
        if (realpath(dir, resolved) != NULL)
            return resolved_is_under_workspace(resolved, actual_ws, wlen);
        if (strcmp(dir, ".") == 0 || strcmp(dir, "/") == 0) return 0;
        if (dir != path_copy)
            snprintf(path_copy, sizeof(path_copy), "%s", dir);
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Public: path-under-workspace check (5.4)                             */
/* ------------------------------------------------------------------ */

int allowlist_path_is_under_workspace(const char *path, const char *workspace_root)
{
    char resolved_path[PATH_MAX];
    char resolved_ws[PATH_MAX];
    const char *actual_ws;
    size_t wlen;
    if (!path || !workspace_root || !workspace_root[0]) return 0;
    /* Resolve the workspace root (handles symlinks like macOS /tmp -> /private/tmp). */
    if (realpath(workspace_root, resolved_ws))
        actual_ws = resolved_ws;
    else
        actual_ws = workspace_root;
    wlen = strlen(actual_ws);
    if (realpath(path, resolved_path))
        return resolved_is_under_workspace(resolved_path, actual_ws, wlen);
    return existing_ancestor_is_under_workspace(path, actual_ws, wlen);
}

/* ------------------------------------------------------------------ */
/* Public: combined check                                               */
/* ------------------------------------------------------------------ */

int allowlist_check_shell_command(const char *cmd, const allowlist_config_t *cfg,
                                  char *reason_buf, size_t reason_cap)
{
    const char *const *p;
    char ws_resolved[PATH_MAX];
    const char *workspace_root = NULL;
    int workspace_only = 0;
    char *cmd_copy = NULL;
    char *tok;
    char *saveptr;
    if (!cmd) {
        set_reason(reason_buf, reason_cap, "null command", "");
        return 1;
    }
    /* Phase 1: built-in substring blocklist */
    for (p = BLOCK_SUBSTRINGS; *p; p++) {
        if (strstr(cmd, *p) != NULL) {
            set_reason(reason_buf, reason_cap,
                       "command blocked: contains forbidden pattern '", *p);
            if (reason_buf && reason_cap > 0) {
                size_t used = strlen(reason_buf);
                if (used + 2 < reason_cap) {
                    reason_buf[used] = '\'';
                    reason_buf[used + 1] = '\0';
                }
            }
            fprintf(stderr, "allowlist: blocked command containing '%s'\n", *p);
            return 1;
        }
    }
    /* Phase 2: workspace path containment */
    if (!cfg || !cfg->workspace_only || !cfg->workspace_path || !cfg->workspace_path[0])
        return 0;
    workspace_only = cfg->workspace_only;
    (void)workspace_only;
    /* Resolve workspace root once */
    if (!realpath(cfg->workspace_path, ws_resolved)) {
        /* Workspace path does not exist; use as-is. */
        size_t n = strlen(cfg->workspace_path);
        if (n >= PATH_MAX) n = PATH_MAX - 1;
        memcpy(ws_resolved, cfg->workspace_path, n);
        ws_resolved[n] = '\0';
    }
    workspace_root = ws_resolved;
    if (block_if_embedded_paths_escape(cmd, workspace_root, reason_buf, reason_cap))
        return 1;
    cmd_copy = strdup(cmd);
    if (!cmd_copy) {
        set_reason(reason_buf, reason_cap, "command blocked: out of memory", "");
        return 1;
    }
    tok = strtok_r(cmd_copy, " \t\n;|&><", &saveptr);
    while (tok) {
        tok = strip_surrounding_quotes(tok);
        if (has_path_chars(tok)) {
            char expanded[PATH_MAX];

            if (expand_shell_path_token(tok, expanded, sizeof(expanded)) != 0) {
                set_reason(reason_buf, reason_cap,
                           "command blocked: unresolved shell path expansion: ", tok);
                fprintf(stderr, "allowlist: blocked unresolved shell path: %s\n", tok);
                free(cmd_copy);
                return 1;
            }
            if (!allowlist_path_is_under_workspace(expanded, workspace_root)) {
                set_reason(reason_buf, reason_cap,
                           "command blocked: path escapes workspace: ", expanded);
                fprintf(stderr, "allowlist: blocked path outside workspace: %s\n", expanded);
                free(cmd_copy);
                return 1;
            }
        }
        tok = strtok_r(NULL, " \t\n;|&><", &saveptr);
    }
    free(cmd_copy);
    return 0;
}
