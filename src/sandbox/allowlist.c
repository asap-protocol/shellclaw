/**
 * @file allowlist.c
 * @brief Shell-command allowlist: blocklist plus workspace path scans.
 *
 * sandbox_exec() does not pivot_root. Landlock is the kernel host-FS bound.
 * This scanner is defense-in-depth for quoted/embedded paths, $HOME/$PWD,
 * file: URLs, and relative symlink tokens the old strtok gate missed.
 */
#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "sandbox/allowlist.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char *const BLOCK_SUBSTRINGS[] = {
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
    "shutdown",
    "reboot",
    "halt",
    "poweroff",
    "init 0",
    "init 6",
    "systemctl poweroff",
    "systemctl reboot",
    "systemctl halt",
    ":(){ :|:& };:",
    "fork()",
    ":(){:|:&};:",
    "chmod 777 /",
    "chmod -R 777 /",
    "chown root",
    "sudo rm -rf",
    "/etc/shadow",
    "/etc/gshadow",
    "~/.ssh/id_",
    "id_rsa",
    "id_ed25519",
    "/dev/nvhost",
    "/dev/nvgpu",
    "/dev/nvmap",
    "/tmp/argus_socket",
    NULL
};

static void set_reason(char *buf, size_t cap, const char *prefix, const char *detail)
{
    if (!buf || cap == 0)
        return;
    if (detail && detail[0])
        snprintf(buf, cap, "%s%s", prefix, detail);
    else
        snprintf(buf, cap, "%s", prefix);
    buf[cap - 1] = '\0';
}

static int has_path_chars(const char *tok)
{
    if (!tok || !tok[0])
        return 0;
    return tok[0] == '/' || tok[0] == '~' || tok[0] == '.' || tok[0] == '$';
}

static int is_option_token(const char *tok)
{
    if (!tok || tok[0] != '-' || tok[1] == '\0')
        return 0;
    if (tok[1] >= '0' && tok[1] <= '9')
        return 0;
    return 1;
}

static int is_path_body_char(unsigned char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '/' || c == '.' || c == '_' ||
           c == '-' || c == '+' || c == '%' || c == '@';
}

static int is_ident_cont(unsigned char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
}

static int join_under_workspace(const char *workspace_root, const char *rel,
                                char *out, size_t cap)
{
    if (!workspace_root || !rel || !out || cap == 0)
        return -1;
    {
        int n = snprintf(out, cap, "%s/%s", workspace_root, rel);
        if (n < 0 || (size_t)n >= cap)
            return -1;
    }
    return 0;
}

static int deny_escaped(char *reason_buf, size_t reason_cap, const char *shown)
{
    set_reason(reason_buf, reason_cap, "command blocked: path escapes workspace: ", shown);
    fprintf(stderr, "allowlist: blocked path outside workspace: %s\n", shown);
    return 1;
}

static int deny_unresolved(char *reason_buf, size_t reason_cap, const char *shown)
{
    set_reason(reason_buf, reason_cap,
               "command blocked: unresolved shell path expansion: ", shown);
    fprintf(stderr, "allowlist: blocked unresolved shell path: %s\n", shown);
    return 1;
}

static int check_joined_or_abs(const char *candidate, const char *workspace_root,
                               char *reason_buf, size_t reason_cap)
{
    char joined[PATH_MAX];
    const char *check = candidate;

    if (!candidate || candidate[0] == '\0')
        return 0;
    if (candidate[0] != '/') {
        if (join_under_workspace(workspace_root, candidate, joined, sizeof(joined)) != 0)
            return deny_escaped(reason_buf, reason_cap, candidate);
        check = joined;
    }
    if (!allowlist_path_is_under_workspace(check, workspace_root))
        return deny_escaped(reason_buf, reason_cap, check);
    return 0;
}

static int block_if_relative_token_escapes(const char *tok, const char *workspace_root,
                                           char *reason_buf, size_t reason_cap)
{
    char joined[PATH_MAX];

    if (!tok || !tok[0] || !workspace_root || !workspace_root[0])
        return 0;
    if (has_path_chars(tok) || is_option_token(tok))
        return 0;
    if (join_under_workspace(workspace_root, tok, joined, sizeof(joined)) != 0)
        return deny_escaped(reason_buf, reason_cap, tok);
    if (access(joined, F_OK) != 0)
        return 0;
    if (!allowlist_path_is_under_workspace(joined, workspace_root))
        return deny_escaped(reason_buf, reason_cap, joined);
    return 0;
}

static char *strip_surrounding_quotes(char *tok)
{
    size_t n;

    if (!tok || !tok[0])
        return tok;
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

    if (strncmp(tok, prefix, prefix_len) != 0)
        return 1;
    suffix = tok + prefix_len;
    if (require_slash_or_end && suffix[0] != '\0' && suffix[0] != '/')
        return 1;
    if (!value)
        return -1;
    {
        int n = snprintf(expanded, expanded_cap, "%s%s", value, suffix);
        if (n < 0 || (size_t)n >= expanded_cap)
            return -1;
    }
    return 0;
}

static int expand_shell_path_token(const char *tok, char *expanded, size_t expanded_cap)
{
    const char *home;
    const char *cwd;
    int rc;

    if (!tok || !expanded || expanded_cap == 0)
        return -1;
    if (tok[0] == '~') {
        int n;
        home = getenv("HOME");
        if (!home || home[0] == '\0')
            return -1;
        n = snprintf(expanded, expanded_cap, "%s%s", home, tok + 1);
        return (n < 0 || (size_t)n >= expanded_cap) ? -1 : 0;
    }
    if (tok[0] != '$') {
        if (strlen(tok) >= expanded_cap)
            return -1;
        memcpy(expanded, tok, strlen(tok) + 1);
        return 0;
    }
    if (tok[1] == '\'' || tok[1] == '"' || tok[1] == '(')
        return -1;
    home = getenv("HOME");
    cwd = getenv("PWD");
    rc = expand_env_prefix(tok, "${HOME}", 7, 0, home, expanded, expanded_cap);
    if (rc != 1)
        return rc;
    rc = expand_env_prefix(tok, "$HOME", 5, 1, home, expanded, expanded_cap);
    if (rc != 1)
        return rc;
    rc = expand_env_prefix(tok, "${PWD}", 6, 0, cwd, expanded, expanded_cap);
    if (rc != 1)
        return rc;
    rc = expand_env_prefix(tok, "$PWD", 4, 1, cwd, expanded, expanded_cap);
    if (rc != 1)
        return rc;
    return -1;
}

static int slash_follows_home_or_pwd_brace(const char *text, const char *slash)
{
    if (!text || !slash || slash <= text)
        return 0;
    if (slash >= text + 7 && strncmp(slash - 7, "${HOME}", 7) == 0)
        return 1;
    if (slash >= text + 6 && strncmp(slash - 6, "${PWD}", 6) == 0)
        return 1;
    return 0;
}

static int is_inside_url(const char *text, const char *p)
{
    const char *q;

    if (!text || !p || p < text)
        return 0;
    for (q = p; q > text; q--) {
        unsigned char c = (unsigned char)q[-1];
        if (c == ' ' || c == '\t' || c == '\n' || c == ';' || c == '|' ||
            c == '&' || c == '<' || c == '>' || c == '"' || c == '\'')
            return 0;
        if (q >= text + 3 && q[-3] == ':' && q[-2] == '/' && q[-1] == '/')
            return 1;
    }
    return 0;
}

static int is_fs_absolute_path_start(const char *text, const char *p)
{
    unsigned char prev;

    if (!text || !p)
        return 0;
    if (*p == '.') {
        if (!(p[1] == '/' ||
              (p[1] == '.' && (p[2] == '/' || p[2] == '\0' ||
                               p[2] == '\'' || p[2] == '"' ||
                               !is_path_body_char((unsigned char)p[2])))))
            return 0;
        if (is_inside_url(text, p) && p[1] != '.')
            return 0;
        if (p == text)
            return 1;
        prev = (unsigned char)p[-1];
        if (is_path_body_char(prev) && prev != '/')
            return 0;
        return 1;
    }
    if (*p != '/' && *p != '~')
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
    if (prev == '}')
        return !slash_follows_home_or_pwd_brace(text, p);
    return 1;
}

static int expand_tilde_fragment(const char *fragment, char *dest, size_t dest_cap)
{
    const char *home;

    if (!fragment || !dest || dest_cap == 0)
        return -1;
    if (fragment[0] != '~') {
        if (strlen(fragment) >= dest_cap)
            return -1;
        memcpy(dest, fragment, strlen(fragment) + 1);
        return 0;
    }
    home = getenv("HOME");
    if (!home || home[0] == '\0')
        return -1;
    {
        int n = snprintf(dest, dest_cap, "%s%s", home, fragment + 1);
        if (n < 0 || (size_t)n >= dest_cap)
            return -1;
    }
    return 0;
}

static int block_if_embedded_paths_escape(const char *text, const char *workspace_root,
                                          char *reason_buf, size_t reason_cap)
{
    const char *p;

    if (!text || !workspace_root)
        return 0;
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
        if (expand_tilde_fragment(fragment, expanded, sizeof(expanded)) != 0)
            return deny_escaped(reason_buf, reason_cap, fragment);
        if (check_joined_or_abs(expanded, workspace_root, reason_buf, reason_cap))
            return 1;
        if (p > start)
            p--;
    }
    return 0;
}

static size_t path_suffix_len(const char *p)
{
    size_t n = 0;

    if (!p || p[0] != '/')
        return 0;
    while (p[n] && is_path_body_char((unsigned char)p[n]))
        n++;
    return n;
}

static int block_expanded_env_path(const char *value, const char *suffix, size_t suffix_len,
                                   const char *workspace_root, const char *raw,
                                   char *reason_buf, size_t reason_cap)
{
    char expanded[PATH_MAX];

    if (!value)
        return deny_unresolved(reason_buf, reason_cap, raw);
    {
        int n = snprintf(expanded, sizeof(expanded), "%s%.*s", value, (int)suffix_len, suffix);
        if (n < 0 || (size_t)n >= sizeof(expanded))
            return deny_unresolved(reason_buf, reason_cap, raw);
    }
    if (!allowlist_path_is_under_workspace(expanded, workspace_root))
        return deny_escaped(reason_buf, reason_cap, expanded);
    return 0;
}

static int block_if_dollar_expansions_escape(const char *text, const char *workspace_root,
                                             char *reason_buf, size_t reason_cap)
{
    const char *p;
    const char *home;
    const char *cwd;

    if (!text || !workspace_root)
        return 0;
    home = getenv("HOME");
    cwd = getenv("PWD");
    for (p = text; *p; ) {
        size_t suffix_n;

        if (*p != '$') {
            p++;
            continue;
        }
        if (p[1] == '\'' || p[1] == '"' || p[1] == '(')
            return deny_unresolved(reason_buf, reason_cap, p);
        if (p[1] == '{') {
            if (strncmp(p, "${HOME}", 7) == 0) {
                suffix_n = path_suffix_len(p + 7);
                if (block_expanded_env_path(home, p + 7, suffix_n, workspace_root, p,
                                            reason_buf, reason_cap))
                    return 1;
                p += 7 + suffix_n;
                continue;
            }
            if (strncmp(p, "${PWD}", 6) == 0) {
                suffix_n = path_suffix_len(p + 6);
                if (block_expanded_env_path(cwd, p + 6, suffix_n, workspace_root, p,
                                            reason_buf, reason_cap))
                    return 1;
                p += 6 + suffix_n;
                continue;
            }
            return deny_unresolved(reason_buf, reason_cap, p);
        }
        if (strncmp(p, "$HOME", 5) == 0 && !is_ident_cont((unsigned char)p[5])) {
            suffix_n = path_suffix_len(p + 5);
            if (block_expanded_env_path(home, p + 5, suffix_n, workspace_root, p,
                                        reason_buf, reason_cap))
                return 1;
            p += 5 + suffix_n;
            continue;
        }
        if (strncmp(p, "$PWD", 4) == 0 && !is_ident_cont((unsigned char)p[4])) {
            suffix_n = path_suffix_len(p + 4);
            if (block_expanded_env_path(cwd, p + 4, suffix_n, workspace_root, p,
                                        reason_buf, reason_cap))
                return 1;
            p += 4 + suffix_n;
            continue;
        }
        return deny_unresolved(reason_buf, reason_cap, p);
    }
    return 0;
}

static int prefix_ci_eq(const char *p, const char *prefix)
{
    size_t i;

    if (!p || !prefix)
        return 0;
    for (i = 0; prefix[i]; i++) {
        unsigned char a = (unsigned char)p[i];
        unsigned char b = (unsigned char)prefix[i];
        if (a >= 'A' && a <= 'Z')
            a = (unsigned char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z')
            b = (unsigned char)(b - 'A' + 'a');
        if (a != b)
            return 0;
    }
    return 1;
}

static int hex_nibble(unsigned char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

static int percent_decode_inplace(char *s)
{
    char *r = s;
    char *w = s;

    while (*r) {
        if (*r == '%') {
            int hi;
            int lo;

            if (!r[1] || !r[2])
                return -1;
            hi = hex_nibble((unsigned char)r[1]);
            lo = hex_nibble((unsigned char)r[2]);
            if (hi < 0 || lo < 0)
                return -1;
            *w++ = (char)((hi << 4) | lo);
            r += 3;
            continue;
        }
        *w++ = *r++;
    }
    *w = '\0';
    return 0;
}

static int block_if_file_url_escapes(const char *text, const char *workspace_root,
                                     char *reason_buf, size_t reason_cap)
{
    const char *p;

    if (!text || !workspace_root)
        return 0;
    for (p = text; *p; p++) {
        const char *s;
        char path[PATH_MAX];
        size_t n = 0;

        if (!prefix_ci_eq(p, "file:"))
            continue;
        if (p > text) {
            unsigned char prev = (unsigned char)p[-1];

            if (is_path_body_char(prev) && prev != '/')
                continue;
        }
        s = p + 5;
        while (*s == '/')
            s++;
        if (prefix_ci_eq(s, "localhost") &&
            (s[9] == '/' || s[9] == '\0' || s[9] == '\'' || s[9] == '"'))
            s += 9;
        else if (strncmp(s, "127.0.0.1", 9) == 0 &&
                 (s[9] == '/' || s[9] == '\0' || s[9] == '\'' || s[9] == '"'))
            s += 9;
        while (*s == '/')
            s++;
        if (*s == '\0' || *s == '\'' || *s == '"')
            continue;
        path[n++] = '/';
        while (*s && is_path_body_char((unsigned char)*s) && n + 1 < sizeof(path))
            path[n++] = *s++;
        path[n] = '\0';
        if (percent_decode_inplace(path) != 0)
            return deny_escaped(reason_buf, reason_cap, path);
        if (!allowlist_path_is_under_workspace(path, workspace_root))
            return deny_escaped(reason_buf, reason_cap, path);
    }
    return 0;
}

static int at_word_start(const char *text, const char *p)
{
    unsigned char prev;

    if (!text || !p)
        return 0;
    if (p == text)
        return 1;
    prev = (unsigned char)p[-1];
    return !is_ident_cont(prev);
}

static int name_is_home_or_pwd(const char *q)
{
    if (strncmp(q, "HOME", 4) == 0 && !is_ident_cont((unsigned char)q[4]))
        return 1;
    if (strncmp(q, "PWD", 3) == 0 && !is_ident_cont((unsigned char)q[3]))
        return 1;
    return 0;
}

static int command_mutates_home_or_pwd(const char *text)
{
    const char *p;

    if (!text)
        return 0;
    for (p = text; *p; p++) {
        const char *q;

        if (!at_word_start(text, p))
            continue;
        if (strncmp(p, "HOME=", 5) == 0 || strncmp(p, "PWD=", 4) == 0)
            return 1;
        if (strncmp(p, "unset", 5) == 0 && !is_ident_cont((unsigned char)p[5])) {
            q = p + 5;
            while (*q == ' ' || *q == '\t')
                q++;
            if (name_is_home_or_pwd(q))
                return 1;
        }
        if (strncmp(p, "export", 6) == 0 && !is_ident_cont((unsigned char)p[6])) {
            q = p + 6;
            while (*q == ' ' || *q == '\t')
                q++;
            if (name_is_home_or_pwd(q))
                return 1;
        }
    }
    return 0;
}

static int blocklist_hit(const char *cmd, char *reason_buf, size_t reason_cap)
{
    const char *const *p;

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
    return 0;
}

int allowlist_check_shell_command(const char *cmd, const allowlist_config_t *cfg,
                                  char *reason_buf, size_t reason_cap)
{
    char ws_resolved[PATH_MAX];
    const char *workspace_root;
    char *cmd_copy;
    char *tok;
    char *saveptr;

    if (!cmd) {
        set_reason(reason_buf, reason_cap, "null command", "");
        return 1;
    }
    if (blocklist_hit(cmd, reason_buf, reason_cap))
        return 1;
    if (!cfg || !cfg->workspace_only || !cfg->workspace_path || !cfg->workspace_path[0])
        return 0;
    if (!realpath(cfg->workspace_path, ws_resolved)) {
        size_t n = strlen(cfg->workspace_path);
        if (n >= PATH_MAX)
            n = PATH_MAX - 1;
        memcpy(ws_resolved, cfg->workspace_path, n);
        ws_resolved[n] = '\0';
    }
    workspace_root = ws_resolved;
    if (command_mutates_home_or_pwd(cmd)) {
        set_reason(reason_buf, reason_cap,
                   "command blocked: HOME/PWD assignment in command", "");
        fprintf(stderr, "allowlist: blocked HOME/PWD assignment in command\n");
        return 1;
    }
    if (block_if_file_url_escapes(cmd, workspace_root, reason_buf, reason_cap))
        return 1;
    if (block_if_dollar_expansions_escape(cmd, workspace_root, reason_buf, reason_cap))
        return 1;
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
                free(cmd_copy);
                return deny_unresolved(reason_buf, reason_cap, tok);
            }
            if (check_joined_or_abs(expanded, workspace_root, reason_buf, reason_cap)) {
                free(cmd_copy);
                return 1;
            }
        } else if (block_if_relative_token_escapes(tok, workspace_root, reason_buf,
                                                   reason_cap)) {
            free(cmd_copy);
            return 1;
        }
        tok = strtok_r(NULL, " \t\n;|&><", &saveptr);
    }
    free(cmd_copy);
    return 0;
}
