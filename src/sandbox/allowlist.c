/**
 * @file allowlist.c
 * @brief Shell-command allowlist: built-in blocklist + workspace realpath checks.
 */
#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "sandbox/allowlist.h"
#include <ctype.h>
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
    "auth_tokens.json",
    "shellclaw.pid",
    "shellclaw.log",
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

/** Return 1 if @p s begins with prefix after any leading whitespace. */
static int has_path_chars(const char *tok)
{
    if (!tok) return 0;
    return tok[0] == '/' || tok[0] == '~' || tok[0] == '.';
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
    if (realpath(path, resolved_path)) {
        /* Exact match or resolved path starts with resolved workspace + '/' */
        if (strncmp(resolved_path, actual_ws, wlen) == 0) {
            if (resolved_path[wlen] == '\0' || resolved_path[wlen] == '/') return 1;
        }
        return 0;
    }
    /* Path does not exist on disk: check the lexical prefix against resolved workspace. */
    if (strncmp(path, actual_ws, wlen) == 0) {
        if (path[wlen] == '\0' || path[wlen] == '/') return 1;
    }
    /* Also try against the original (unresolved) workspace root. */
    wlen = strlen(workspace_root);
    if (strncmp(path, workspace_root, wlen) == 0) {
        if (path[wlen] == '\0' || path[wlen] == '/') return 1;
    }
    return 0;
}

int allowlist_path_is_runtime_state_file(const char *path)
{
    char resolved[PATH_MAX];
    const char *use = path;
    const char *base;
    const char *slash;
    char parent[PATH_MAX];
    size_t parent_len;

    if (!path || !path[0])
        return 0;
    if (realpath(path, resolved) != NULL)
        use = resolved;
    base = strrchr(use, '/');
    base = base ? base + 1 : use;
    if (strcmp(base, "auth_tokens.json") == 0 ||
        strcmp(base, "shellclaw.pid") == 0 ||
        strcmp(base, "shellclaw.log") == 0)
        return 1;
    if (strcmp(base, "config.toml") != 0 && strcmp(base, "memory.db") != 0 &&
        strncmp(base, "memory.db-", 10) != 0)
        return 0;
    slash = strrchr(use, '/');
    if (!slash || slash == use)
        return 0;
    parent_len = (size_t)(slash - use);
    if (parent_len >= sizeof(parent))
        return 0;
    memcpy(parent, use, parent_len);
    parent[parent_len] = '\0';
    slash = strrchr(parent, '/');
    slash = slash ? slash + 1 : parent;
    return strcmp(slash, ".shellclaw") == 0;
}

/** Copy @p rel under @p root, collapsing "." and "..". Absolute @p rel is copied as-is. */
static int join_under_root(const char *root, const char *rel, char *out, size_t cap)
{
    char tmp[PATH_MAX];
    char *dup;
    char *save = NULL;
    char *tok;
    char *stack[48] = {0};
    int nstack = 0;
    int i;
    size_t used;

    if (!rel || !rel[0] || !out || cap == 0)
        return -1;
    if (rel[0] == '/') {
        if (strlen(rel) + 1 > cap)
            return -1;
        memcpy(out, rel, strlen(rel) + 1);
        return 0;
    }
    if (!root || !root[0])
        return -1;
    if (snprintf(tmp, sizeof(tmp), "%s/%s", root, rel) >= (int)sizeof(tmp))
        return -1;
    dup = strdup(tmp);
    if (!dup)
        return -1;
    for (tok = strtok_r(dup, "/", &save); tok; tok = strtok_r(NULL, "/", &save)) {
        if (strcmp(tok, ".") == 0)
            continue;
        if (strcmp(tok, "..") == 0) {
            if (nstack > 0)
                nstack--;
            continue;
        }
        if (nstack >= (int)(sizeof(stack) / sizeof(stack[0]))) {
            free(dup);
            return -1;
        }
        stack[nstack++] = tok;
    }
    used = 0;
    out[0] = '\0';
    for (i = 0; i < nstack; i++) {
        size_t part = strlen(stack[i]);
        if (used + 1 + part + 1 > cap) {
            free(dup);
            return -1;
        }
        out[used++] = '/';
        memcpy(out + used, stack[i], part);
        used += part;
        out[used] = '\0';
    }
    free(dup);
    if (used == 0)
        return -1;
    return 0;
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
    /* Phase 2: runtime-state paths, then optional workspace containment.
     * State files are rejected even when workspace_only is off, so an
     * unsandboxed `cat ~/.shellclaw/config.toml` cannot skip the check. */
    workspace_only = cfg && cfg->workspace_only && cfg->workspace_path &&
                     cfg->workspace_path[0];
    if (cfg && cfg->workspace_path && cfg->workspace_path[0]) {
        if (!realpath(cfg->workspace_path, ws_resolved)) {
            size_t n = strlen(cfg->workspace_path);
            if (n >= PATH_MAX) n = PATH_MAX - 1;
            memcpy(ws_resolved, cfg->workspace_path, n);
            ws_resolved[n] = '\0';
        }
        workspace_root = ws_resolved;
    }
    /* Tokenize the command and check each path-like token. */
    cmd_copy = strdup(cmd);
    if (!cmd_copy) return 0; /* fail-open on OOM */
    tok = strtok_r(cmd_copy, " \t\n;|&><", &saveptr);
    while (tok) {
        char expanded[PATH_MAX];
        const char *check = tok;
        if (tok[0] == '~') {
            const char *home = getenv("HOME");
            if (home)
                snprintf(expanded, sizeof(expanded), "%s%s", home, tok + 1);
            else
                snprintf(expanded, sizeof(expanded), "%s", tok);
            check = expanded;
        }
        if (allowlist_path_is_runtime_state_file(check)) {
            set_reason(reason_buf, reason_cap,
                       "command blocked: runtime state file: ", check);
            fprintf(stderr, "allowlist: blocked runtime state file: %s\n", check);
            free(cmd_copy);
            return 1;
        }
        /* sandbox_exec chdirs into the workspace, so a bare name is that file. */
        if (workspace_root && check[0] != '/') {
            char joined[PATH_MAX];
            if (join_under_root(workspace_root, check, joined, sizeof(joined)) == 0 &&
                allowlist_path_is_runtime_state_file(joined)) {
                set_reason(reason_buf, reason_cap,
                           "command blocked: runtime state file: ", joined);
                fprintf(stderr, "allowlist: blocked runtime state file: %s\n", joined);
                free(cmd_copy);
                return 1;
            }
        }
        if (workspace_only && has_path_chars(tok)) {
            if (!allowlist_path_is_under_workspace(check, workspace_root)) {
                set_reason(reason_buf, reason_cap,
                           "command blocked: path escapes workspace: ", check);
                fprintf(stderr, "allowlist: blocked path outside workspace: %s\n", check);
                free(cmd_copy);
                return 1;
            }
        }
        tok = strtok_r(NULL, " \t\n;|&><", &saveptr);
    }
    free(cmd_copy);
    return 0;
}
