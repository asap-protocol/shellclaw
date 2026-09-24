/**
 * @file allowlist_path.c
 * @brief Workspace containment for shell-command paths (defense-in-depth).
 *
 * realpath(3) cannot canonicalize a missing destination. Walking the first
 * existing ancestor (same idea as tools/file.c) still collapses `..` through
 * directories that exist. Lexical collapse runs first so a missing component
 * before `..` cannot pin the walk at the workspace. `..` is not cancelled
 * across a symlink: the kernel walks the link first.
 */
#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "sandbox/allowlist.h"
#include <libgen.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int resolved_is_under_workspace(const char *resolved, const char *actual_ws,
                                       size_t wlen)
{
    if (!resolved || !actual_ws || wlen == 0)
        return 0;
    if (strncmp(resolved, actual_ws, wlen) != 0)
        return 0;
    return resolved[wlen] == '\0' || resolved[wlen] == '/';
}

static int existing_ancestor_is_under_workspace(const char *path, const char *actual_ws,
                                                size_t wlen)
{
    char path_copy[PATH_MAX];
    char parent[PATH_MAX];
    char resolved[PATH_MAX];
    int hops;

    if (!path || path[0] == '\0' || strlen(path) >= PATH_MAX)
        return 0;
    snprintf(path_copy, sizeof(path_copy), "%s", path);
    for (hops = 0; hops < PATH_MAX; hops++) {
        char *dir;
        size_t n;

        dir = dirname(path_copy);
        if (!dir || dir[0] == '\0')
            return 0;
        n = strlen(dir);
        if (n >= sizeof(parent))
            return 0;
        memcpy(parent, dir, n + 1);
        if (realpath(parent, resolved) != NULL)
            return resolved_is_under_workspace(resolved, actual_ws, wlen);
        if (strcmp(parent, ".") == 0 || strcmp(parent, "/") == 0)
            return 0;
        memcpy(path_copy, parent, n + 1);
    }
    return 0;
}

static int append_path_seg(char *buf, size_t cap, size_t *len, const char *seg,
                           int add_slash)
{
    size_t sl;

    if (!buf || !len || !seg)
        return -1;
    sl = strlen(seg);
    if (add_slash) {
        if (*len + 1 >= cap)
            return -1;
        buf[(*len)++] = '/';
    }
    if (*len + sl + 1 > cap)
        return -1;
    memcpy(buf + *len, seg, sl);
    *len += sl;
    buf[*len] = '\0';
    return 0;
}

/** True when the stacked path is a symlink, so `..` must not cancel it. */
static int stacked_path_is_symlink(const char **parts, int nparts, int absolute)
{
    char probe[PATH_MAX];
    struct stat st;
    size_t probe_len;
    int pi;

    memset(&st, 0, sizeof(st));
    if (absolute) {
        probe[0] = '/';
        probe_len = 1;
        probe[1] = '\0';
    } else {
        probe_len = 0;
        probe[0] = '\0';
    }
    for (pi = 0; pi < nparts; pi++) {
        if (!parts[pi])
            return 1;
        if (append_path_seg(probe, sizeof(probe), &probe_len, parts[pi],
                            (absolute && pi == 0) ? 0 : (probe_len > 0)) != 0)
            return 1;
    }
    return lstat(probe, &st) == 0 && S_ISLNK(st.st_mode);
}

/**
 * Collapse `.` / `..` without requiring directories to exist, so
 * `/ws/nope/../../../tmp/x` becomes `/tmp/x` instead of walking back to `/ws`.
 */
static int lexical_collapse_path(const char *path, char *out, size_t out_cap)
{
    char tmp[PATH_MAX];
    const char *parts[PATH_MAX / 2];
    int nparts = 0;
    int absolute;
    size_t len;
    char *cur;
    int i;
    size_t out_len;

    memset(parts, 0, sizeof(parts));
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
                if (stacked_path_is_symlink(parts, nparts, absolute))
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
        out[1] = '\0';
    } else {
        out_len = 0;
        out[0] = '\0';
    }
    if (!absolute && nparts == 0) {
        if (out_cap < 2)
            return -1;
        out[0] = '.';
        out[1] = '\0';
        return 0;
    }
    for (i = 0; i < nparts; i++) {
        if (!parts[i])
            return -1;
        if (append_path_seg(out, out_cap, &out_len, parts[i],
                            (!absolute && i == 0) ? 0 : (out_len > (absolute ? 1 : 0))) != 0)
            return -1;
    }
    return 0;
}

int allowlist_path_is_under_workspace(const char *path, const char *workspace_root)
{
    char resolved_path[PATH_MAX];
    char resolved_ws[PATH_MAX];
    char collapsed[PATH_MAX];
    const char *actual_ws;
    size_t wlen;

    if (!path || !workspace_root || !workspace_root[0])
        return 0;
    if (realpath(workspace_root, resolved_ws))
        actual_ws = resolved_ws;
    else
        actual_ws = workspace_root;
    wlen = strlen(actual_ws);
    if (realpath(path, resolved_path))
        return resolved_is_under_workspace(resolved_path, actual_ws, wlen);
    if (lexical_collapse_path(path, collapsed, sizeof(collapsed)) != 0)
        return 0;
    if (realpath(collapsed, resolved_path))
        return resolved_is_under_workspace(resolved_path, actual_ws, wlen);
    return existing_ancestor_is_under_workspace(collapsed, actual_ws, wlen);
}
