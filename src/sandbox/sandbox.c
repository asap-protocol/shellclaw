/**
 * @file sandbox.c
 * @brief Process sandbox: Linux namespaces, cgroups v2, timeout.
 *
 * Linux path: fork the isolator, then unshare mount/network/PID (user ns
 * first when unprivileged). Isolation failure is reported on a control pipe,
 * not via sh-compatible exit codes. dup2, chdir, PID-1 fork, and workspace
 * setup failures write that byte before exiting; a 0-byte read is success.
 * After CLONE_NEWPID, fork so the command
 * is PID 1 (unshare does not move the caller). That child fchdir's the
 * workspace, remounts procfs, sets PR_SET_PDEATHSIG, and closes fds >= 3
 * so a timeout SIGKILL of the isolator cannot leave the command under
 * host init. The isolator joins its cgroup before the command fork so
 * memory.max and cpu.max apply to sh -c. Limits degrade if unavailable.
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
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#endif

#define DEFAULT_TIMEOUT_MS     10000
#define DEFAULT_MEMORY_MAX     (64UL * 1024UL * 1024UL)
#define DEFAULT_CGROUP_BASE    "/sys/fs/cgroup"
#define CGROUP_NAME_PREFIX     "shellclaw_sb_"
#define PIPE_POLL_SLICE_MS     500
#define SANDBOX_ISO_NS         1
#define SANDBOX_ISO_LL         2

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

static int cgroup_prepare(const char *base, const char *name,
                          size_t memory_max, const char *cpu_max_str)
{
    char cpath[1024];
    char val[64];
    int n;

    n = snprintf(cpath, sizeof(cpath), "%s/%s", base, name);
    if (n < 0 || (size_t)n >= sizeof(cpath))
        return -1;
    if (mkdir(cpath, 0755) != 0 && errno != EEXIST)
        return -1;
    snprintf(val, sizeof(val), "%zu", memory_max > 0 ? memory_max : DEFAULT_MEMORY_MAX);
    cgroup_write_file(cpath, "memory.max", val);
    if (cpu_max_str && cpu_max_str[0])
        cgroup_write_file(cpath, "cpu.max", cpu_max_str);
    return 0;
}

/* cgroup v2 does not move descendants that already exist. Write 0 before fork. */
static int cgroup_join_self(const char *cpath)
{
    if (!cpath || !cpath[0])
        return -1;
    return cgroup_write_file(cpath, "cgroup.procs", "0");
}

static void cgroup_remove(const char *base, const char *name)
{
    char cpath[1024];
    snprintf(cpath, sizeof(cpath), "%s/%s", base, name);
    rmdir(cpath);
}

#endif /* __linux__ */

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
    if (unshare(CLONE_NEWNS | CLONE_NEWNET | CLONE_NEWPID) != 0)
        return -1;
    /* Shared mounts would let the later umount2("/proc") hit the host. */
    if (mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) != 0)
        return -1;
    return 0;
}

static int isolate_namespaces(void)
{
    if (unshare_isolation_namespaces() == 0)
        return 0;
    if (enter_user_namespace() != 0)
        return -1;
    return unshare_isolation_namespaces();
}

static int remount_procfs(void)
{
    (void)umount2("/proc", MNT_DETACH);
    if (mount("proc", "/proc", "proc", MS_NOSUID | MS_NOEXEC | MS_NODEV, NULL) != 0)
        return -1;
    return 0;
}

static int enter_workspace_cwd(const char *workspace)
{
    int ws_fd;
    ws_fd = open(workspace, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (ws_fd < 0)
        return -1;
    if (fchdir(ws_fd) != 0) {
        close(ws_fd);
        return -1;
    }
    close(ws_fd);
    return 0;
}

static unsigned char setup_command_process(const char *workspace)
{
    if (prctl(PR_SET_PDEATHSIG, SIGKILL, 0, 0, 0) != 0)
        return SANDBOX_ISO_NS;
    if (workspace && workspace[0] && enter_workspace_cwd(workspace) != 0)
        return SANDBOX_ISO_NS;
    if (remount_procfs() != 0)
        return SANDBOX_ISO_NS;
    return 0;
}

#endif /* __linux__ */

static void close_inherited_fds(void)
{
#if defined(__linux__) && defined(__NR_close_range)
    if (syscall(__NR_close_range, 3, ~0U, 0) == 0)
        return;
#endif
    {
        int fd;
        for (fd = 3; fd < 1024; fd++)
            (void)close(fd);
    }
}

static ssize_t read_isolation_byte(int fd, unsigned char *iso, int timeout_ms)
{
    struct pollfd pfd;
    int r;
    pfd.fd = fd;
    pfd.events = POLLIN;
    pfd.revents = 0;
    r = poll(&pfd, 1, timeout_ms);
    if (r <= 0)
        return -1;
    {
        ssize_t n = read(fd, iso, 1);
        return n;
    }
}

static unsigned char setup_child_process(int pipe_wr, const char *workspace)
{
    close(STDIN_FILENO);
    if (dup2(pipe_wr, STDOUT_FILENO) < 0)
        return SANDBOX_ISO_NS;
    if (dup2(pipe_wr, STDERR_FILENO) < 0)
        return SANDBOX_ISO_NS;
    close(pipe_wr);
#ifdef __linux__
    (void)workspace;
    (void)setsid();
    if (isolate_namespaces() != 0)
        return SANDBOX_ISO_NS;
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0)
        return SANDBOX_ISO_NS;
    if (prctl(PR_SET_PDEATHSIG, SIGKILL, 0, 0, 0) != 0)
        return SANDBOX_ISO_NS;
#else
    if (workspace && workspace[0] && chdir(workspace) != 0)
        return SANDBOX_ISO_NS;
#endif
    return 0;
}

#ifdef __linux__
static void wait_and_exit_with_child(pid_t cmd_pid)
{
    int st = 0;

    if (waitpid(cmd_pid, &st, 0) < 0)
        _exit(125);
    if (WIFEXITED(st))
        _exit(WEXITSTATUS(st));
    if (WIFSIGNALED(st))
        _exit(128 + WTERMSIG(st));
    _exit(1);
}
#endif

static int reap_child(pid_t pid, int *status_out)
{
    int st = 0;
    int wr = waitpid(pid, &st, WNOHANG);
    if (wr == 0) {
        struct timespec ts;
        int retries;
        kill(pid, SIGKILL);
        (void)kill(-pid, SIGKILL);
        for (retries = 0; retries < 40; retries++) {
            wr = waitpid(pid, &st, WNOHANG);
            if (wr != 0) break;
            ts.tv_sec = 0;
            ts.tv_nsec = 50 * 1000 * 1000;
            nanosleep(&ts, NULL);
        }
        if (wr == 0) waitpid(pid, &st, 0);
        if (status_out) *status_out = st;
        return 1;
    }
    if (status_out) *status_out = st;
    return 0;
}

static int report_isolation_failure(char *out, size_t out_cap, unsigned char iso)
{
    if (iso == SANDBOX_ISO_LL)
        snprintf(out, out_cap, "sandbox: Landlock filesystem bound failed");
    else
        snprintf(out, out_cap, "sandbox: namespace isolation failed");
    return -1;
}

static int close_pipes_pair(int a, int b)
{
    close(a);
    close(b);
    return -1;
}

int sandbox_exec(const char *cmd, char *out, size_t out_cap,
                 int timeout_ms, const sandbox_config_t *cfg)
{
    int pipefd[2];
    int errpipe[2];
    pid_t pid;
    size_t total;
    int timed_out = 0;
    const char *workspace = cfg ? cfg->workspace_path : NULL;
    int used_cgroup = 0;
#ifdef __linux__
    char cgroup_name[80];
    char cgroup_dir[1024];
    const char *cgroup_base = (cfg && cfg->cgroup_base && cfg->cgroup_base[0])
                               ? cfg->cgroup_base : DEFAULT_CGROUP_BASE;
    size_t memory_max = cfg ? cfg->memory_max_bytes : 0;
    const char *cpu_max_str = cfg ? cfg->cpu_max : NULL;
    static unsigned cgroup_seq;
    cgroup_dir[0] = '\0';
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
    if (pipe(errpipe) != 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }
    if (fcntl(pipefd[0], F_SETFD, FD_CLOEXEC) != 0 ||
        fcntl(errpipe[0], F_SETFD, FD_CLOEXEC) != 0 ||
        fcntl(errpipe[1], F_SETFD, FD_CLOEXEC) != 0) {
        close(errpipe[0]);
        close(errpipe[1]);
        return close_pipes_pair(pipefd[0], pipefd[1]);
    }
#ifdef __linux__
    if (cgroup_controllers_available(cgroup_base)) {
        cgroup_seq++;
        snprintf(cgroup_name, sizeof(cgroup_name), "%s%d_%u",
                 CGROUP_NAME_PREFIX, (int)getpid(), cgroup_seq);
        if (cgroup_prepare(cgroup_base, cgroup_name, memory_max, cpu_max_str) == 0) {
            int n = snprintf(cgroup_dir, sizeof(cgroup_dir), "%s/%s",
                             cgroup_base, cgroup_name);
            if (n > 0 && (size_t)n < sizeof(cgroup_dir))
                used_cgroup = 1;
        } else {
            fprintf(stderr, "sandbox: cgroup setup failed (non-fatal)\n");
        }
    }
#endif
    pid = fork();
    if (pid < 0) {
        close(errpipe[0]);
        close(errpipe[1]);
#ifdef __linux__
        if (used_cgroup)
            cgroup_remove(cgroup_base, cgroup_name);
#endif
        return close_pipes_pair(pipefd[0], pipefd[1]);
    }
    if (pid == 0) {
        unsigned char iso;
        close(pipefd[0]);
        close(errpipe[0]);
#ifdef __linux__
        if (used_cgroup && cgroup_join_self(cgroup_dir) != 0)
            fprintf(stderr, "sandbox: cgroup join failed (non-fatal)\n");
#endif
        iso = setup_child_process(pipefd[1], workspace);
        if (iso != 0) {
            if (write(errpipe[1], &iso, 1) < 0) { /* parent fail-closes on EOF */ }
            _exit(1);
        }
#ifdef __linux__
        {
            pid_t cmd_pid = fork();
            if (cmd_pid < 0) {
                unsigned char fork_iso = SANDBOX_ISO_NS;
                if (write(errpipe[1], &fork_iso, 1) < 0) { /* parent fail-closes on EOF */ }
                _exit(1);
            }
            if (cmd_pid > 0) {
                close(errpipe[1]);
                wait_and_exit_with_child(cmd_pid);
            }
            iso = setup_command_process(workspace);
            if (iso != 0) {
                if (write(errpipe[1], &iso, 1) < 0) { /* parent fail-closes on EOF */ }
                _exit(1);
            }
        }
#endif
        close(errpipe[1]);
        close_inherited_fds();
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }
    close(pipefd[1]);
    close(errpipe[1]);
    {
        unsigned char iso = 0;
        ssize_t n = read_isolation_byte(errpipe[0], &iso, timeout_ms);
        close(errpipe[0]);
        if (n != 0) {
            (void)reap_child(pid, NULL);
#ifdef __linux__
            if (used_cgroup)
                cgroup_remove(cgroup_base, cgroup_name);
#endif
            close(pipefd[0]);
            if (n != 1)
                iso = SANDBOX_ISO_NS;
            return report_isolation_failure(out, out_cap, iso);
        }
    }
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
