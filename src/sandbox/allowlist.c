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
#include <sys/stat.h>
#include <unistd.h>

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

static int is_ident_start(unsigned char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

static int is_ident_cont(unsigned char c)
{
    return is_ident_start(c) || (c >= '0' && c <= '9');
}

static int is_cmd_word_start(const char *text, const char *p)
{
    unsigned char prev;

    if (!text || !p || p < text)
        return 0;
    if (p == text)
        return 1;
    prev = (unsigned char)p[-1];
    return prev == ' ' || prev == '\t' || prev == '\n' || prev == '\r' ||
           prev == ';' || prev == '|' || prev == '&' || prev == '(' ||
           prev == '{' || prev == ')';
}

static int name_is_home_or_pwd(const char *p, size_t *nlen)
{
    if (strncmp(p, "HOME", 4) == 0 && !is_ident_cont((unsigned char)p[4])) {
        if (nlen)
            *nlen = 4;
        return 1;
    }
    if (strncmp(p, "PWD", 3) == 0 && !is_ident_cont((unsigned char)p[3])) {
        if (nlen)
            *nlen = 3;
        return 1;
    }
    return 0;
}

/**
 * Process getenv(HOME/PWD) is wrong after `PWD=; cat $PWD/etc/passwd`.
 * Fail closed when the command text assigns, exports, or unsets those names.
 */
static int command_mutates_home_or_pwd(const char *text)
{
    const char *p;

    if (!text)
        return 0;
    for (p = text; *p; p++) {
        size_t nlen = 0;

        if (!is_cmd_word_start(text, p))
            continue;
        if (name_is_home_or_pwd(p, &nlen) && p[nlen] == '=')
            return 1;
        if (strncmp(p, "unset", 5) == 0 && !is_ident_cont((unsigned char)p[5])) {
            const char *q = p + 5;

            while (*q == ' ' || *q == '\t')
                q++;
            while (*q && *q != ';' && *q != '|' && *q != '&' && *q != '\n') {
                if (name_is_home_or_pwd(q, &nlen))
                    return 1;
                while (*q && *q != ' ' && *q != '\t' && *q != ';' &&
                       *q != '|' && *q != '&' && *q != '\n')
                    q++;
                while (*q == ' ' || *q == '\t')
                    q++;
            }
        }
        if (strncmp(p, "export", 6) == 0 && !is_ident_cont((unsigned char)p[6])) {
            const char *q = p + 6;

            while (*q == ' ' || *q == '\t')
                q++;
            while (*q && *q != ';' && *q != '|' && *q != '&' && *q != '\n') {
                if (name_is_home_or_pwd(q, &nlen))
                    return 1;
                while (*q && *q != ' ' && *q != '\t' && *q != ';' &&
                       *q != '|' && *q != '&' && *q != '\n')
                    q++;
                while (*q == ' ' || *q == '\t')
                    q++;
            }
        }
    }
    return 0;
}

/**
 * Bytes of a leading-slash escape that decodes to `/` (`\x2f`, `\u002f`,
 * `\U0000002f`, octal `\57` / `\057`). Not a Python interpreter: `chr(47)`
 * with no slash encoding in the text is still out of scope.
 */
static size_t encoded_leading_slash_len(const char *p)
{
    if (!p || p[0] != '\\' || p[1] == '\0')
        return 0;
    if ((p[1] == 'x' || p[1] == 'X') && p[2] == '2' &&
        (p[3] == 'f' || p[3] == 'F'))
        return 4;
    if (p[1] == 'u' && p[2] == '0' && p[3] == '0' && p[4] == '2' &&
        (p[5] == 'f' || p[5] == 'F'))
        return 6;
    if (p[1] == 'U' && p[2] == '0' && p[3] == '0' && p[4] == '0' &&
        p[5] == '0' && p[6] == '0' && p[7] == '0' && p[8] == '2' &&
        (p[9] == 'f' || p[9] == 'F'))
        return 10;
    if (p[1] >= '0' && p[1] <= '7') {
        int val = 0;
        size_t n = 0;

        while (n < 3 && p[1 + n] >= '0' && p[1 + n] <= '7') {
            val = val * 8 + (p[1 + n] - '0');
            n++;
            if (val == 47)
                return 1 + n;
        }
    }
    return 0;
}

static int hex_nibble(unsigned char c)
{
    if (c >= '0' && c <= '9')
        return (int)(c - '0');
    if (c >= 'a' && c <= 'f')
        return (int)(c - 'a' + 10);
    if (c >= 'A' && c <= 'F')
        return (int)(c - 'A' + 10);
    return -1;
}

/** Percent-decode @p s in place. Invalid `%` and `%00` fail closed. */
static int percent_decode_inplace(char *s)
{
    char *r;
    char *w;

    if (!s)
        return -1;
    r = s;
    w = s;
    while (*r) {
        if (r[0] == '%') {
            int hi;
            int lo;
            unsigned char v;

            hi = hex_nibble((unsigned char)r[1]);
            lo = hex_nibble((unsigned char)r[2]);
            if (hi < 0 || lo < 0)
                return -1;
            v = (unsigned char)((hi << 4) | lo);
            if (v == 0)
                return -1;
            *w++ = (char)v;
            r += 3;
            continue;
        }
        *w++ = *r++;
    }
    *w = '\0';
    return 0;
}

/** `${HOME}/` and `${PWD}/` are one expansion; `${IFS}/` is a new FS root. */
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

/** True when @p p sits in a `://` URL span (so `https://.../../` is not a host path). */
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
        if (is_inside_url(text, p))
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
    if (!home || home[0] == '\0')
        return -1;
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
        if (expanded[0] != '/') {
            char joined[PATH_MAX];
            int jn;

            jn = snprintf(joined, sizeof(joined), "%s/%s", workspace_root, expanded);
            if (jn < 0 || (size_t)jn >= sizeof(joined)) {
                set_reason(reason_buf, reason_cap,
                           "command blocked: path escapes workspace: ", fragment);
                return 1;
            }
            memcpy(expanded, joined, (size_t)jn + 1);
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

/**
 * Reconstruct `/` + the following path body after `\x2f` / `\57` / `\u002f`
 * so a later literal slash in `etc/passwd` cannot hide the encoded root.
 */
static int block_if_encoded_slash_escapes(const char *text, const char *workspace_root,
                                          char *reason_buf, size_t reason_cap)
{
    const char *p;

    if (!text || !workspace_root) return 0;
    for (p = text; *p; ) {
        size_t esc;
        size_t n;
        const char *body;
        char reconstructed[PATH_MAX];

        esc = encoded_leading_slash_len(p);
        if (!esc) {
            p++;
            continue;
        }
        body = p + esc;
        reconstructed[0] = '/';
        n = 1;
        while (*body && is_path_body_char((unsigned char)*body) &&
               n + 1 < sizeof(reconstructed))
            reconstructed[n++] = *body++;
        reconstructed[n] = '\0';
        if (!allowlist_path_is_under_workspace(reconstructed, workspace_root)) {
            set_reason(reason_buf, reason_cap,
                       "command blocked: path escapes workspace: ", reconstructed);
            fprintf(stderr, "allowlist: blocked path outside workspace: %s\n",
                    reconstructed);
            return 1;
        }
        p = body;
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
    int n;

    if (!value) {
        set_reason(reason_buf, reason_cap,
                   "command blocked: unresolved shell path expansion: ", raw);
        fprintf(stderr, "allowlist: blocked unresolved shell path: %s\n", raw);
        return 1;
    }
    n = snprintf(expanded, sizeof(expanded), "%s%.*s", value, (int)suffix_len, suffix);
    if (n < 0 || (size_t)n >= sizeof(expanded)) {
        set_reason(reason_buf, reason_cap,
                   "command blocked: unresolved shell path expansion: ", raw);
        fprintf(stderr, "allowlist: blocked unresolved shell path: %s\n", raw);
        return 1;
    }
    if (!allowlist_path_is_under_workspace(expanded, workspace_root)) {
        set_reason(reason_buf, reason_cap,
                   "command blocked: path escapes workspace: ", expanded);
        fprintf(stderr, "allowlist: blocked path outside workspace: %s\n", expanded);
        return 1;
    }
    return 0;
}

/**
 * Scan `$` on the full command. Shell glues expansions onto the previous word
 * (`cat$IFS/etc/passwd`, `cat"$HOME/.bashrc"`), so strtok + tok[0]=='$' misses them.
 * Expand `$HOME` / `${HOME}` / `$PWD` / `${PWD}` (plus a following `/...` suffix);
 * fail closed on ANSI-C, command substitution, `$IFS`, and other `$...` forms.
 */
static int block_if_dollar_expansions_escape(const char *text, const char *workspace_root,
                                             char *reason_buf, size_t reason_cap)
{
    const char *p;
    const char *home;
    const char *cwd;

    if (!text || !workspace_root) return 0;
    home = getenv("HOME");
    cwd = getenv("PWD");
    for (p = text; *p; ) {
        size_t suffix_n;

        if (*p != '$') {
            p++;
            continue;
        }
        if (p[1] == '\'' || p[1] == '"' || p[1] == '(') {
            set_reason(reason_buf, reason_cap,
                       "command blocked: unresolved shell path expansion: ", p);
            fprintf(stderr, "allowlist: blocked unresolved shell path: %s\n", p);
            return 1;
        }
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
            set_reason(reason_buf, reason_cap,
                       "command blocked: unresolved shell path expansion: ", p);
            fprintf(stderr, "allowlist: blocked unresolved shell path: %s\n", p);
            return 1;
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
        set_reason(reason_buf, reason_cap,
                   "command blocked: unresolved shell path expansion: ", p);
        fprintf(stderr, "allowlist: blocked unresolved shell path: %s\n", p);
        return 1;
    }
    return 0;
}

static int prefix_ci_eq(const char *p, const char *prefix)
{
    size_t i;

    for (i = 0; prefix[i]; i++) {
        unsigned char a = (unsigned char)p[i];
        unsigned char b = (unsigned char)prefix[i];

        if (a >= 'A' && a <= 'Z') a = (unsigned char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = (unsigned char)(b - 'A' + 'a');
        if (a != b) return 0;
    }
    return 1;
}

/**
 * `is_fs_absolute_path_start` skips `/` after `:`, so `file:/etc/passwd` and
 * `file://localhost/etc/passwd` never start a path fragment. Extract the local
 * path from `file:` URLs, percent-decode, and run the workspace check.
 */
static int block_if_file_url_escapes(const char *text, const char *workspace_root,
                                     char *reason_buf, size_t reason_cap)
{
    const char *p;

    if (!text || !workspace_root) return 0;
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
        if (prefix_ci_eq(s, "localhost") && (s[9] == '/' || s[9] == '\0' ||
                                             s[9] == '\'' || s[9] == '"'))
            s += 9;
        else if (strncmp(s, "127.0.0.1", 9) == 0 &&
                 (s[9] == '/' || s[9] == '\0' || s[9] == '\'' || s[9] == '"'))
            s += 9;
        else if (strncmp(s, "[::1]", 5) == 0 &&
                 (s[5] == '/' || s[5] == '\0' || s[5] == '\'' || s[5] == '"'))
            s += 5;
        while (*s == '/')
            s++;
        if (*s == '\0' || *s == '\'' || *s == '"')
            continue;
        path[n++] = '/';
        while (*s && is_path_body_char((unsigned char)*s) && n + 1 < sizeof(path))
            path[n++] = *s++;
        path[n] = '\0';
        if (percent_decode_inplace(path) != 0) {
            set_reason(reason_buf, reason_cap,
                       "command blocked: path escapes workspace: ", path);
            fprintf(stderr, "allowlist: blocked invalid percent-encoded file URL\n");
            return 1;
        }
        if (!allowlist_path_is_under_workspace(path, workspace_root)) {
            set_reason(reason_buf, reason_cap,
                       "command blocked: path escapes workspace: ", path);
            fprintf(stderr, "allowlist: blocked path outside workspace: %s\n", path);
            return 1;
        }
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
    char parent[PATH_MAX];
    char resolved[PATH_MAX];
    int hops;

    if (!path || path[0] == '\0' || strlen(path) >= PATH_MAX) return 0;
    snprintf(path_copy, sizeof(path_copy), "%s", path);
    for (hops = 0; hops < PATH_MAX; hops++) {
        char *dir;
        size_t n;

        dir = dirname(path_copy);
        if (!dir || dir[0] == '\0') return 0;
        n = strlen(dir);
        if (n >= sizeof(parent)) return 0;
        memcpy(parent, dir, n + 1);
        if (realpath(parent, resolved) != NULL)
            return resolved_is_under_workspace(resolved, actual_ws, wlen);
        if (strcmp(parent, ".") == 0 || strcmp(parent, "/") == 0) return 0;
        memcpy(path_copy, parent, n + 1);
    }
    return 0;
}

/**
 * Collapse `.` / `..` without requiring directories to exist, so
 * `/ws/nope/../../../tmp/x` becomes `/tmp/x` instead of walking back to `/ws`.
 * Do not cancel `..` across a symlink: the kernel walks the link first, so
 * `workspace/out/../etc/passwd` with `out` -> `/` is `/etc/passwd`.
 */
static int lexical_collapse_path(const char *path, char *out, size_t out_cap)
{
    char tmp[PATH_MAX];
    const char *parts[PATH_MAX / 2] = { NULL };
    int nparts = 0;
    int absolute;
    size_t len;
    char *cur;
    int i;
    size_t out_len;

    if (!path || !out || out_cap == 0)
        return -1;
    len = strlen(path);
    if (len == 0 || len >= sizeof(tmp))
        return -1;
    memcpy(tmp, path, len + 1);
    absolute = (tmp[0] == '/');
    cur = absolute ? tmp + 1 : tmp;
    while (*cur) {
        char *seg = cur;

        while (*cur && *cur != '/')
            cur++;
        if (*cur == '/') {
            *cur = '\0';
            cur++;
        }
        if (seg[0] == '\0' || strcmp(seg, ".") == 0)
            continue;
        if (strcmp(seg, "..") == 0) {
            if (nparts > 0) {
                char probe[PATH_MAX];
                struct stat st;
                size_t probe_len;
                int pi;

                memset(&st, 0, sizeof(st));

                if (absolute) {
                    probe[0] = '/';
                    probe_len = 1;
                } else {
                    probe_len = 0;
                }
                for (pi = 0; pi < nparts; pi++) {
                    const char *ps = parts[pi];
                    size_t sl;

                    if (!ps)
                        return -1;
                    sl = strlen(ps);
                    if (pi > 0) {
                        if (probe_len + 1 >= sizeof(probe))
                            return -1;
                        probe[probe_len++] = '/';
                    }
                    if (probe_len + sl + 1 > sizeof(probe))
                        return -1;
                    memcpy(probe + probe_len, ps, sl);
                    probe_len += sl;
                }
                probe[probe_len] = '\0';
                if (lstat(probe, &st) == 0 && S_ISLNK(st.st_mode))
                    return -1;
                nparts--;
            }
            continue;
        }
        if (nparts >= (int)(sizeof(parts) / sizeof(parts[0])))
            return -1;
        parts[nparts++] = seg;
    }
    if (absolute) {
        if (out_cap < 2)
            return -1;
        out[0] = '/';
        out_len = 1;
    } else {
        out_len = 0;
    }
    for (i = 0; i < nparts; i++) {
        const char *seg = parts[i];
        size_t sl;

        if (!seg)
            return -1;
        sl = strlen(seg);

        if (i > 0) {
            if (out_len + 1 >= out_cap)
                return -1;
            out[out_len++] = '/';
        }
        if (out_len + sl + 1 > out_cap)
            return -1;
        memcpy(out + out_len, seg, sl);
        out_len += sl;
    }
    if (!absolute && nparts == 0) {
        if (out_cap < 2)
            return -1;
        out[0] = '.';
        out[1] = '\0';
        return 0;
    }
    out[out_len] = '\0';
    return 0;
}

/* ------------------------------------------------------------------ */
/* Public: path-under-workspace check (5.4)                             */
/* ------------------------------------------------------------------ */

int allowlist_path_is_under_workspace(const char *path, const char *workspace_root)
{
    char resolved_path[PATH_MAX];
    char resolved_ws[PATH_MAX];
    char collapsed[PATH_MAX];
    const char *actual_ws;
    size_t wlen;
    if (!path || !workspace_root || !workspace_root[0]) return 0;
    /* Resolve the workspace root (handles symlinks like macOS /tmp -> /private/tmp). */
    if (realpath(workspace_root, resolved_ws))
        actual_ws = resolved_ws;
    else
        actual_ws = workspace_root;
    wlen = strlen(actual_ws);
    /* Kernel walk first so symlink/.. matches open(2), not lexical pop. */
    if (realpath(path, resolved_path))
        return resolved_is_under_workspace(resolved_path, actual_ws, wlen);
    if (lexical_collapse_path(path, collapsed, sizeof(collapsed)) != 0)
        return 0;
    if (realpath(collapsed, resolved_path))
        return resolved_is_under_workspace(resolved_path, actual_ws, wlen);
    return existing_ancestor_is_under_workspace(collapsed, actual_ws, wlen);
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
    /* Resolve workspace root once */
    if (!realpath(cfg->workspace_path, ws_resolved)) {
        /* Workspace path does not exist; use as-is. */
        size_t n = strlen(cfg->workspace_path);
        if (n >= PATH_MAX) n = PATH_MAX - 1;
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
    if (block_if_dollar_expansions_escape(cmd, workspace_root, reason_buf, reason_cap))
        return 1;
    if (block_if_file_url_escapes(cmd, workspace_root, reason_buf, reason_cap))
        return 1;
    if (block_if_encoded_slash_escapes(cmd, workspace_root, reason_buf, reason_cap))
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
