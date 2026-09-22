/**
 * @file sandbox.c
 * @brief Process sandbox: Linux namespace isolation, cgroups v2, timeout/kill.
 *
 * Linux path: fork() + unshare(CLONE_NEWNS | CLONE_NEWNET | CLONE_NEWPID) in the
 * child, giving the shell and its children mount, network, and PID namespace
 * isolation respectively.  Does not mount(2), bind-mount, or pivot_root(2); Jetson
 * GPU nodes (/dev/nvhost-*, /dev/nvgpu, /dev/nvmap) are never injected into the
 * namespace — see docs/SECURITY.md.  cgroups v2 memory.max and cpu.max limits are applied
 * via the host cgroup hierarchy when available; the function degrades gracefully
 * if the kernel does not expose writable cgroup controllers.
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
#endif

#define DEFAULT_TIMEOUT_MS     10000
#define DEFAULT_MEMORY_MAX     (64UL * 1024UL * 1024UL)  /* 64 MiB */
#define DEFAULT_CGROUP_BASE    "/sys/fs/cgroup"
#define CGROUP_NAME_PREFIX     "shellclaw_sb_"
#define PIPE_POLL_SLICE_MS     500

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

static void setup_child_process(int pipe_wr, const char *workspace)
{
    close(STDIN_FILENO);
    if (dup2(pipe_wr, STDOUT_FILENO) < 0) _exit(125);
    if (dup2(pipe_wr, STDERR_FILENO) < 0) _exit(125);
    close(pipe_wr);
#ifdef __linux__
    setsid();
    /* Namespace isolation: mount + network + PID (children of this process). */
    unshare(CLONE_NEWNS | CLONE_NEWNET | CLONE_NEWPID);
    prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0);
#endif
    if (workspace && workspace[0])
        if (chdir(workspace) != 0) _exit(124);
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
    timed_out = reap_child(pid, NULL);
    if (timed_out && total < out_cap - 40)
        snprintf(out + total, out_cap - total, "\n[Sandbox: command timed out after %d ms]",
                 timeout_ms);
#ifdef __linux__
    if (used_cgroup)
        cgroup_remove(cgroup_base, cgroup_name);
#else
    (void)used_cgroup;
#endif
    return 0;
}
