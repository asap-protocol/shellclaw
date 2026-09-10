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

/**
 * Return 1 if @p tok looks like a filesystem path (or expands to one).
 * Includes shell parameter expansions such as `$HOME/...` and `${PWD}/...`
 * that never start with `/` `~` `.` but become absolute after /bin/sh expands them.
 */
static int has_path_chars(const char *tok)
{
    if (!tok || !tok[0]) return 0;
    if (tok[0] == '/' || tok[0] == '~' || tok[0] == '.') return 1;
    /* `$HOME`, `${HOME}/x`, `$PWD/../out`, `$(pwd)/x`, ANSI-C `$'\x2f...'`. */
    if (tok[0] == '$') return 1;
    return 0;
}

/**
 * Expand a leading `~`, `$HOME` / `${HOME}`, or `$PWD` / `${PWD}` for workspace checks.
 * Unknown `$...` forms fail closed (return -1) so workspace_only cannot be bypassed
 * via arbitrary parameter / command substitutions.
 * @return 0 and writes into @p expanded on success; -1 if the token must be blocked.
 */
static int expand_shell_path_token(const char *tok, char *expanded, size_t expanded_cap)
{
    const char *home;
    const char *cwd;
    const char *suffix;
    int n;

    if (!tok || !expanded || expanded_cap == 0) return -1;

    if (tok[0] == '~') {
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

    /* Fail closed on ANSI-C quoting and command substitution (encoded / dynamic paths). */
    if (tok[1] == '\'' || tok[1] == '"' || tok[1] == '(')
        return -1;

    home = getenv("HOME");
    cwd = getenv("PWD");
    if (strncmp(tok, "${HOME}", 7) == 0) {
        suffix = tok + 7;
        if (!home) return -1;
        n = snprintf(expanded, expanded_cap, "%s%s", home, suffix);
        return (n < 0 || (size_t)n >= expanded_cap) ? -1 : 0;
    }
    if (strncmp(tok, "$HOME", 5) == 0 && (tok[5] == '\0' || tok[5] == '/')) {
        suffix = tok + 5;
        if (!home) return -1;
        n = snprintf(expanded, expanded_cap, "%s%s", home, suffix);
        return (n < 0 || (size_t)n >= expanded_cap) ? -1 : 0;
    }
    if (strncmp(tok, "${PWD}", 6) == 0) {
        suffix = tok + 6;
        if (!cwd) return -1;
        n = snprintf(expanded, expanded_cap, "%s%s", cwd, suffix);
        return (n < 0 || (size_t)n >= expanded_cap) ? -1 : 0;
    }
    if (strncmp(tok, "$PWD", 4) == 0 && (tok[4] == '\0' || tok[4] == '/')) {
        suffix = tok + 4;
        if (!cwd) return -1;
        n = snprintf(expanded, expanded_cap, "%s%s", cwd, suffix);
        return (n < 0 || (size_t)n >= expanded_cap) ? -1 : 0;
    }

    /* Other `$VAR` / `${VAR}` forms: fail closed under workspace_only. */
    return -1;
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
    /* Tokenize the command and check each path-like token. */
    cmd_copy = strdup(cmd);
    if (!cmd_copy) return 0; /* fail-open on OOM */
    tok = strtok_r(cmd_copy, " \t\n;|&><", &saveptr);
    while (tok) {
        if (has_path_chars(tok)) {
            char expanded[PATH_MAX];
            const char *check_path = tok;
            if (expand_shell_path_token(tok, expanded, sizeof(expanded)) != 0) {
                set_reason(reason_buf, reason_cap,
                           "command blocked: unresolved shell path expansion: ", tok);
                fprintf(stderr, "allowlist: blocked unresolved shell path: %s\n", tok);
                free(cmd_copy);
                return 1;
            }
            check_path = expanded;
            if (!allowlist_path_is_under_workspace(check_path, workspace_root)) {
                set_reason(reason_buf, reason_cap,
                           "command blocked: path escapes workspace: ", check_path);
                fprintf(stderr, "allowlist: blocked path outside workspace: %s\n", check_path);
                free(cmd_copy);
                return 1;
            }
        }
        tok = strtok_r(NULL, " \t\n;|&><", &saveptr);
    }
    free(cmd_copy);
    return 0;
}
