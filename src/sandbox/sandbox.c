/**
 * @file sandbox.c
 * @brief Process sandbox: Linux namespace isolation, cgroups v2, timeout/kill.
 *
 * Linux path: fork() then, in the child, unshare mount/network/PID namespaces
 * (entering a user namespace first when unprivileged). Isolation failure is
 * fail-closed (_exit SANDBOX_EXIT_NO_NS). When workspace_path is set, Landlock
 * is the kernel FS bound (fail-closed SANDBOX_EXIT_NO_LL); the allowlist scanner
 * is defense-in-depth only. cgroups v2 limits degrade if unavailable.
 *
 * Non-Linux path: plain fork() + execl(); a warning is emitted to stderr.
 */
#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "sandbox/sandbox.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifdef __linux__
#include <sched.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <linux/landlock.h>
#endif

#define DEFAULT_TIMEOUT_MS     10000
#define DEFAULT_MEMORY_MAX     (64UL * 1024UL * 1024UL)  /* 64 MiB */
#define DEFAULT_CGROUP_BASE    "/sys/fs/cgroup"
#define CGROUP_NAME_PREFIX     "shellclaw_sb_"
#define PIPE_POLL_SLICE_MS     500
/* Child exits when isolation cannot be applied. Distinct from 124 (chdir),
 * 125 (dup2), and 127 (exec). */
#define SANDBOX_EXIT_NO_NS     123
#define SANDBOX_EXIT_NO_LL     122

/* ------------------------------------------------------------------ */
/* cgroups v2 helpers (Linux only)                                      */
/* ------------------------------------------------------------------ */

#ifdef __linux__

static int cgroup_write_file(const char *dir, const char *filename, const char *value)
{
    char path[1280];
    int fd;
    ssize_t n;
    snprintf(path, sizeof(path), "%s/%s", dir, filename);
    fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    n = write(fd, value, strlen(value));
    close(fd);
    return (n < 0) ? -1 : 0;
}

static int cgroup_controllers_available(const char *base)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/cgroup.controllers", base);
    return access(path, F_OK) == 0;
}

/**
 * Create a cgroup at <base>/<name>, write resource limits, assign @p pid.
 * Returns 0 on success; the caller must call cgroup_remove() when done.
 */
static int cgroup_create(const char *base, const char *name, pid_t pid,
                         size_t memory_max, const char *cpu_max_str)
{
    char cpath[1024];
    char val[64];
    snprintf(cpath, sizeof(cpath), "%s/%s", base, name);
    if (mkdir(cpath, 0755) != 0 && errno != EEXIST) return -1;
    snprintf(val, sizeof(val), "%zu", memory_max > 0 ? memory_max : DEFAULT_MEMORY_MAX);
    cgroup_write_file(cpath, "memory.max", val);
    if (cpu_max_str && cpu_max_str[0])
        cgroup_write_file(cpath, "cpu.max", cpu_max_str);
    snprintf(val, sizeof(val), "%d", (int)pid);
    return cgroup_write_file(cpath, "cgroup.procs", val);
}

static void cgroup_remove(const char *base, const char *name)
{
    char cpath[1024];
    snprintf(cpath, sizeof(cpath), "%s/%s", base, name);
    rmdir(cpath);
}

#endif /* __linux__ */

/* ------------------------------------------------------------------ */
/* Pipe drain with timeout                                              */
/* ------------------------------------------------------------------ */

static size_t drain_pipe(int fd, char *buf, size_t cap, int timeout_ms)
{
    size_t total = 0;
    int elapsed_ms = 0;
    char chunk[512];
    if (cap == 0) return 0;
    while (total < cap - 1 && elapsed_ms < timeout_ms) {
        struct pollfd pfd;
        int slice;
        int r;
        ssize_t n;
        pfd.fd = fd;
        pfd.events = POLLIN;
        pfd.revents = 0;
        slice = timeout_ms - elapsed_ms;
        if (slice > PIPE_POLL_SLICE_MS) slice = PIPE_POLL_SLICE_MS;
        r = poll(&pfd, 1, slice);
        if (r < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (r == 0) {
            elapsed_ms += slice;
            continue;
        }
        n = read(fd, chunk, sizeof(chunk));
        if (n <= 0) break;
        {
            size_t add = (size_t)n;
            if (total + add >= cap - 1) add = cap - 1 - total;
            memcpy(buf + total, chunk, add);
            total += add;
        }
    }
    buf[total] = '\0';
    return total;
}

/* ------------------------------------------------------------------ */
/* Child setup before exec                                              */
/* ------------------------------------------------------------------ */

#ifdef __linux__

static int write_proc_str(const char *path, const char *s)
{
    int fd;
    ssize_t n;
    size_t len;

    fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    len = strlen(s);
    n = write(fd, s, len);
    close(fd);
    return (n == (ssize_t)len) ? 0 : -1;
}

static int enter_user_namespace(void)
{
    char map[64];
    uid_t uid = getuid();
    gid_t gid = getgid();

    if (unshare(CLONE_NEWUSER) != 0) return -1;
    if (write_proc_str("/proc/self/setgroups", "deny\n") != 0) return -1;
    snprintf(map, sizeof map, "0 %u 1\n", (unsigned)uid);
    if (write_proc_str("/proc/self/uid_map", map) != 0) return -1;
    snprintf(map, sizeof map, "0 %u 1\n", (unsigned)gid);
    if (write_proc_str("/proc/self/gid_map", map) != 0) return -1;
    return 0;
}

static int unshare_isolation_namespaces(void)
{
    return unshare(CLONE_NEWNS | CLONE_NEWNET | CLONE_NEWPID);
}

static void isolate_or_exit(void)
{
    if (unshare_isolation_namespaces() == 0) return;
    if (enter_user_namespace() != 0) _exit(SANDBOX_EXIT_NO_NS);
    if (unshare_isolation_namespaces() != 0) _exit(SANDBOX_EXIT_NO_NS);
}

static __u64 landlock_abi1_fs_rights(void)
{
    return LANDLOCK_ACCESS_FS_EXECUTE |
           LANDLOCK_ACCESS_FS_WRITE_FILE |
           LANDLOCK_ACCESS_FS_READ_FILE |
           LANDLOCK_ACCESS_FS_READ_DIR |
           LANDLOCK_ACCESS_FS_REMOVE_DIR |
           LANDLOCK_ACCESS_FS_REMOVE_FILE |
           LANDLOCK_ACCESS_FS_MAKE_CHAR |
           LANDLOCK_ACCESS_FS_MAKE_DIR |
           LANDLOCK_ACCESS_FS_MAKE_REG |
           LANDLOCK_ACCESS_FS_MAKE_SOCK |
           LANDLOCK_ACCESS_FS_MAKE_FIFO |
           LANDLOCK_ACCESS_FS_MAKE_BLOCK |
           LANDLOCK_ACCESS_FS_MAKE_SYM;
}

/**
 * Probe Landlock ABI and mask handled FS rights the running kernel understands.
 * Passing REFER (ABI 2) or TRUNCATE (ABI 3) on ABI 1 makes create_ruleset fail.
 */
static int landlock_handled_fs(__u64 *handled_out)
{
    int abi;
    __u64 handled;

    if (!handled_out) return -1;
    abi = (int)syscall(__NR_landlock_create_ruleset, NULL, 0,
                       LANDLOCK_CREATE_RULESET_VERSION);
    if (abi < 1) return -1;
    handled = landlock_abi1_fs_rights();
#ifdef LANDLOCK_ACCESS_FS_REFER
    if (abi >= 2)
        handled |= LANDLOCK_ACCESS_FS_REFER;
#endif
#ifdef LANDLOCK_ACCESS_FS_TRUNCATE
    if (abi >= 3)
        handled |= LANDLOCK_ACCESS_FS_TRUNCATE;
#endif
    *handled_out = handled;
    return 0;
}

static int landlock_add_path(int ruleset_fd, const char *path, __u64 dir_access,
                             __u64 file_access)
{
    int pfd;
    struct stat st;
    struct landlock_path_beneath_attr pb;
    long rc;

    pfd = open(path, O_PATH | O_CLOEXEC);
    if (pfd < 0)
        return 0;
    memset(&pb, 0, sizeof(pb));
    pb.parent_fd = pfd;
    if (fstat(pfd, &st) == 0 && S_ISDIR(st.st_mode))
        pb.allowed_access = dir_access;
    else
        pb.allowed_access = file_access;
    rc = syscall(__NR_landlock_add_rule, ruleset_fd, LANDLOCK_RULE_PATH_BENEATH,
                 &pb, 0);
    close(pfd);
    (void)rc;
    return 0;
}

static int landlock_add_workspace(int ruleset_fd, const char *workspace, __u64 access)
{
    int ws_fd;
    struct landlock_path_beneath_attr pb;
    long rc;

    ws_fd = open(workspace, O_PATH | O_DIRECTORY | O_CLOEXEC);
    if (ws_fd < 0)
        return -1;
    memset(&pb, 0, sizeof(pb));
    pb.allowed_access = access;
    pb.parent_fd = ws_fd;
    rc = syscall(__NR_landlock_add_rule, ruleset_fd, LANDLOCK_RULE_PATH_BENEATH,
                 &pb, 0);
    close(ws_fd);
    return (rc == 0) ? 0 : -1;
}

/**
 * Landlock FS bound: full access under @p workspace, read/exec for paths
 * needed by /bin/sh and interpreters. Fail closed — do not exec on the host
 * tree if the ruleset cannot be applied.
 */
static int landlock_restrict_to_workspace(const char *workspace)
{
    static const char *const RO_PATHS[] = {
        "/bin", "/usr", "/usr/local", "/lib", "/lib64", "/lib32",
        "/etc/ld.so.cache", "/etc/ld.so.conf", "/etc/ld.so.conf.d",
        "/etc/ssl", "/etc/nsswitch.conf", "/etc/hosts", "/etc/resolv.conf",
        "/dev/null", "/dev/zero", "/dev/urandom", "/dev/tty",
        "/proc",
        NULL
    };
    __u64 handled;
    __u64 workspace_access;
    __u64 ro_dir;
    __u64 ro_file;
    struct landlock_ruleset_attr attr;
    int ruleset_fd;
    size_t i;
    long rc;

    if (!workspace || !workspace[0])
        return 0;
    if (landlock_handled_fs(&handled) != 0)
        return -1;
    workspace_access = (LANDLOCK_ACCESS_FS_EXECUTE |
                        LANDLOCK_ACCESS_FS_WRITE_FILE |
                        LANDLOCK_ACCESS_FS_READ_FILE |
                        LANDLOCK_ACCESS_FS_READ_DIR |
                        LANDLOCK_ACCESS_FS_REMOVE_DIR |
                        LANDLOCK_ACCESS_FS_REMOVE_FILE |
                        LANDLOCK_ACCESS_FS_MAKE_DIR |
                        LANDLOCK_ACCESS_FS_MAKE_REG |
                        LANDLOCK_ACCESS_FS_MAKE_SYM |
                        LANDLOCK_ACCESS_FS_MAKE_FIFO |
                        LANDLOCK_ACCESS_FS_MAKE_SOCK) & handled;
#ifdef LANDLOCK_ACCESS_FS_REFER
    workspace_access |= (LANDLOCK_ACCESS_FS_REFER & handled);
#endif
#ifdef LANDLOCK_ACCESS_FS_TRUNCATE
    workspace_access |= (LANDLOCK_ACCESS_FS_TRUNCATE & handled);
#endif
    ro_dir = (LANDLOCK_ACCESS_FS_EXECUTE |
              LANDLOCK_ACCESS_FS_READ_FILE |
              LANDLOCK_ACCESS_FS_READ_DIR) & handled;
    ro_file = (LANDLOCK_ACCESS_FS_EXECUTE |
               LANDLOCK_ACCESS_FS_READ_FILE) & handled;
    memset(&attr, 0, sizeof(attr));
    attr.handled_access_fs = handled;
    /* ABI-1 field size so older kernels do not return E2BIG. */
    ruleset_fd = (int)syscall(__NR_landlock_create_ruleset, &attr,
                              sizeof(attr.handled_access_fs), 0);
    if (ruleset_fd < 0)
        return -1;
    if (landlock_add_workspace(ruleset_fd, workspace, workspace_access) != 0) {
        close(ruleset_fd);
        return -1;
    }
    for (i = 0; RO_PATHS[i]; i++)
        (void)landlock_add_path(ruleset_fd, RO_PATHS[i], ro_dir, ro_file);
    rc = syscall(__NR_landlock_restrict_self, ruleset_fd, 0);
    close(ruleset_fd);
    return (rc == 0) ? 0 : -1;
}

#endif /* __linux__ */

static void setup_child_process(int pipe_wr, const char *workspace)
{
    close(STDIN_FILENO);
    if (dup2(pipe_wr, STDOUT_FILENO) < 0) _exit(125);
    if (dup2(pipe_wr, STDERR_FILENO) < 0) _exit(125);
    close(pipe_wr);
#ifdef __linux__
    setsid();
    isolate_or_exit();
    prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0);
#endif
    if (workspace && workspace[0]) {
#ifdef __linux__
        if (landlock_restrict_to_workspace(workspace) != 0)
            _exit(SANDBOX_EXIT_NO_LL);
#endif
        if (chdir(workspace) != 0) _exit(124);
    }
}

/* ------------------------------------------------------------------ */
/* Post-drain wait: kill child if still alive                           */
/* ------------------------------------------------------------------ */

static int reap_child(pid_t pid, int *status_out)
{
    int st = 0;
    int wr = waitpid(pid, &st, WNOHANG);
    if (wr == 0) {
        struct timespec ts;
        int retries;
        kill(pid, SIGKILL);
        for (retries = 0; retries < 40; retries++) {
            wr = waitpid(pid, &st, WNOHANG);
            if (wr != 0) break;
            ts.tv_sec = 0;
            ts.tv_nsec = 50 * 1000 * 1000; /* 50 ms */
            nanosleep(&ts, NULL);
        }
        if (wr == 0) waitpid(pid, &st, 0);
        if (status_out) *status_out = st;
        return 1; /* did time out */
    }
    if (status_out) *status_out = st;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Public API                                                           */
/* ------------------------------------------------------------------ */

int sandbox_exec(const char *cmd, char *out, size_t out_cap,
                 int timeout_ms, const sandbox_config_t *cfg)
{
    int pipefd[2];
    pid_t pid;
    size_t total;
    int timed_out = 0;
    int child_st = 0;
    const char *workspace = cfg ? cfg->workspace_path : NULL;
    int used_cgroup = 0;
#ifdef __linux__
    char cgroup_name[80];
    const char *cgroup_base = (cfg && cfg->cgroup_base && cfg->cgroup_base[0])
                               ? cfg->cgroup_base : DEFAULT_CGROUP_BASE;
    size_t memory_max = cfg ? cfg->memory_max_bytes : 0;
    const char *cpu_max_str = cfg ? cfg->cpu_max : NULL;
#endif
    if (!cmd || !out || out_cap == 0) return -1;
    out[0] = '\0';
    if (timeout_ms <= 0) timeout_ms = DEFAULT_TIMEOUT_MS;
#ifndef __linux__
    {
        static int warned = 0;
        if (!warned) {
            fprintf(stderr,
                "sandbox: namespace isolation unavailable on this platform; using plain fork\n");
            warned = 1;
        }
    }
#endif
    if (pipe(pipefd) != 0) return -1;
    if (fcntl(pipefd[0], F_SETFD, FD_CLOEXEC) != 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }
    /* Write end must NOT have CLOEXEC so child inherits it. */
    pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }
    if (pid == 0) {
        /* Child */
        close(pipefd[0]);
        setup_child_process(pipefd[1], workspace);
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }
    /* Parent */
    close(pipefd[1]);
#ifdef __linux__
    if (cgroup_controllers_available(cgroup_base)) {
        snprintf(cgroup_name, sizeof(cgroup_name), "%s%d", CGROUP_NAME_PREFIX, (int)pid);
        if (cgroup_create(cgroup_base, cgroup_name, pid, memory_max, cpu_max_str) == 0)
            used_cgroup = 1;
        else
            fprintf(stderr, "sandbox: cgroup setup failed for pid %d (non-fatal)\n", (int)pid);
    }
#endif
    total = drain_pipe(pipefd[0], out, out_cap, timeout_ms);
    close(pipefd[0]);
    timed_out = reap_child(pid, &child_st);
    if (timed_out && total < out_cap - 40)
        snprintf(out + total, out_cap - total, "\n[Sandbox: command timed out after %d ms]",
                 timeout_ms);
#ifdef __linux__
    {
        int isolation_failed = 0;
        int exit_st = 0;

        if (!timed_out && WIFEXITED(child_st)) {
            exit_st = WEXITSTATUS(child_st);
            if (exit_st == SANDBOX_EXIT_NO_NS || exit_st == SANDBOX_EXIT_NO_LL)
                isolation_failed = 1;
        }
        if (isolation_failed) {
            if (exit_st == SANDBOX_EXIT_NO_LL)
                snprintf(out, out_cap, "sandbox: Landlock filesystem bound failed");
            else
                snprintf(out, out_cap, "sandbox: namespace isolation failed");
        }
        if (used_cgroup)
            cgroup_remove(cgroup_base, cgroup_name);
        if (isolation_failed)
            return -1;
    }
#else
    (void)used_cgroup;
    (void)child_st;
#endif
    return 0;
}
