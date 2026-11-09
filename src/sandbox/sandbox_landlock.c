/**
 * @file sandbox_landlock.c
 * @brief Linux Landlock ruleset: RW workspace, RO system paths, RW /dev/null.
 *
 * Probe the kernel ABI and pass only bits it understands. create_ruleset uses
 * the ABI-1 attr size so older kernels are not E2BIG. Missing optional RO
 * paths are skipped; the workspace rule and restrict_self fail closed.
 * ABI below 3 cannot handle TRUNCATE (JetPack 6.2 is kernel 5.15, ABI 1),
 * so restrict_self is followed by a seccomp denial of the path truncate(2).
 * ftruncate(2) stays allowed: it needs a writable fd, and WRITE_FILE already
 * rejects opening a host file. Shell '>' uses open(O_TRUNC), not truncate(2).
 */
#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "sandbox/sandbox_landlock.h"

#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef __linux__
#include <linux/landlock.h>
#include <sys/syscall.h>
#endif

#ifndef __linux__
int sandbox_landlock_restrict_for_abi(const char *workspace, int abi)
{
    (void)workspace;
    (void)abi;
    return 0;
}

int sandbox_landlock_restrict_to_workspace(const char *workspace)
{
    return sandbox_landlock_restrict_for_abi(workspace, 0);
}

int sandbox_landlock_prepare(const char *workspace)
{
    (void)workspace;
    return 0;
}
#else

#include <errno.h>
#include <stddef.h>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <sys/prctl.h>

#ifndef LANDLOCK_ACCESS_FS_IOCTL_DEV
#define LANDLOCK_ACCESS_FS_IOCTL_DEV (1ULL << 15)
#endif

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

static int landlock_probe_abi(void)
{
    int abi;

    abi = (int)syscall(__NR_landlock_create_ruleset, NULL, 0,
                       LANDLOCK_CREATE_RULESET_VERSION);
    if (abi < 1)
        return -1;
    return abi;
}

/* @p abi selects bits the kernel understands. ABI below 3 has no TRUNCATE. */
static int landlock_handled_fs(int abi, __u64 *handled_out)
{
    __u64 handled;

    if (!handled_out || abi < 1)
        return -1;
    handled = landlock_abi1_fs_rights();
#ifdef LANDLOCK_ACCESS_FS_REFER
    if (abi >= 2)
        handled |= LANDLOCK_ACCESS_FS_REFER;
#endif
#ifdef LANDLOCK_ACCESS_FS_TRUNCATE
    if (abi >= 3)
        handled |= LANDLOCK_ACCESS_FS_TRUNCATE;
#endif
    if (abi >= 5)
        handled |= LANDLOCK_ACCESS_FS_IOCTL_DEV;
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
    return (rc == 0) ? 0 : -1;
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

static __u64 mask_workspace_access(__u64 handled)
{
    __u64 access;

    access = (LANDLOCK_ACCESS_FS_EXECUTE |
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
    access |= (LANDLOCK_ACCESS_FS_REFER & handled);
#endif
#ifdef LANDLOCK_ACCESS_FS_TRUNCATE
    access |= (LANDLOCK_ACCESS_FS_TRUNCATE & handled);
#endif
    return access;
}

#define LANDLOCK_FD_SKIP (-2)

static int landlock_make_ruleset(const char *workspace, int abi)
{
    static const char *const RO_PATHS[] = {
        "/bin", "/usr", "/usr/local", "/lib", "/lib64", "/lib32",
        "/etc/ld.so.cache", "/etc/ld.so.conf", "/etc/ld.so.conf.d",
        /* Public CA bundle only. /etc/ssl/private stays outside the ruleset. */
        "/etc/ssl/certs", "/etc/nsswitch.conf", "/etc/hosts", "/etc/resolv.conf",
        "/proc",
        NULL
    };
    static const char *const RW_DEV[] = {
        "/dev/null", "/dev/zero", "/dev/urandom", "/dev/tty",
        NULL
    };
    __u64 handled;
    __u64 workspace_access;
    __u64 ro_dir;
    __u64 ro_file;
    __u64 rw_file;
    struct landlock_ruleset_attr attr;
    int ruleset_fd;
    size_t i;

    if (!workspace || !workspace[0])
        return LANDLOCK_FD_SKIP;
    if (landlock_handled_fs(abi, &handled) != 0)
        return -1;
    workspace_access = mask_workspace_access(handled);
    ro_dir = (LANDLOCK_ACCESS_FS_EXECUTE |
              LANDLOCK_ACCESS_FS_READ_FILE |
              LANDLOCK_ACCESS_FS_READ_DIR) & handled;
    ro_file = (LANDLOCK_ACCESS_FS_EXECUTE |
               LANDLOCK_ACCESS_FS_READ_FILE) & handled;
    rw_file = (LANDLOCK_ACCESS_FS_EXECUTE |
               LANDLOCK_ACCESS_FS_READ_FILE |
               LANDLOCK_ACCESS_FS_WRITE_FILE) & handled;
#ifdef LANDLOCK_ACCESS_FS_TRUNCATE
    rw_file |= (LANDLOCK_ACCESS_FS_TRUNCATE & handled);
#endif
    memset(&attr, 0, sizeof(attr));
    attr.handled_access_fs = handled;
    ruleset_fd = (int)syscall(__NR_landlock_create_ruleset, &attr,
                              sizeof(attr.handled_access_fs), 0);
    if (ruleset_fd < 0)
        return -1;
    if (landlock_add_workspace(ruleset_fd, workspace, workspace_access) != 0) {
        close(ruleset_fd);
        return -1;
    }
    /* Traverse-only `/` so execl("/bin/sh") can walk to RO trees. READ_DIR
     * does not grant READ_FILE, so /etc/passwd stays closed. */
    if (landlock_add_workspace(ruleset_fd, "/",
                               LANDLOCK_ACCESS_FS_READ_DIR & handled) != 0) {
        close(ruleset_fd);
        return -1;
    }
    for (i = 0; RO_PATHS[i]; i++) {
        if (landlock_add_path(ruleset_fd, RO_PATHS[i], ro_dir, ro_file) != 0) {
            close(ruleset_fd);
            return -1;
        }
    }
    for (i = 0; RW_DEV[i]; i++) {
        if (landlock_add_path(ruleset_fd, RW_DEV[i], ro_dir, rw_file) != 0) {
            close(ruleset_fd);
            return -1;
        }
    }
    return ruleset_fd;
}

static unsigned int native_audit_arch(void)
{
#if defined(__x86_64__)
    return AUDIT_ARCH_X86_64;
#elif defined(__aarch64__)
    return AUDIT_ARCH_AARCH64;
#elif defined(__arm__)
    return AUDIT_ARCH_ARM;
#else
    return 0;
#endif
}

/* Path truncate(2) is outside Landlock until ABI 3. Deny that syscall only. */
static int deny_path_truncate(void)
{
    struct sock_filter filter[] = {
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, arch)),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 0, 1, 0),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | (EPERM & SECCOMP_RET_DATA)),
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, nr)),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_truncate, 0, 1),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | (EPERM & SECCOMP_RET_DATA)),
#ifdef __NR_truncate64
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_truncate64, 0, 1),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | (EPERM & SECCOMP_RET_DATA)),
#endif
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
    };
    struct sock_fprog prog;
    unsigned int arch;

    arch = native_audit_arch();
    if (arch == 0)
        return -1;
    /* Instruction 1 is the architecture compare. A wrong index denies every syscall. */
    filter[1].k = arch;
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0)
        return -1;
    memset(&prog, 0, sizeof(prog));
    prog.len = (unsigned short)(sizeof(filter) / sizeof(filter[0]));
    prog.filter = filter;
    if (prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &prog, 0L, 0L) != 0)
        return -1;
    return 0;
}

int sandbox_landlock_prepare(const char *workspace)
{
    int abi;
    int fd;

    if (!workspace || !workspace[0])
        return 0;
    abi = landlock_probe_abi();
    if (abi < 0)
        return -1;
    fd = landlock_make_ruleset(workspace, abi);
    if (fd < 0)
        return -1;
    close(fd);
    return 0;
}

int sandbox_landlock_restrict_for_abi(const char *workspace, int abi)
{
    int fd;
    long rc;

    if (!workspace || !workspace[0])
        return 0;
    if (abi <= 0) {
        abi = landlock_probe_abi();
        if (abi < 0)
            return -1;
    }
    fd = landlock_make_ruleset(workspace, abi);
    if (fd < 0)
        return -1;
    rc = syscall(__NR_landlock_restrict_self, fd, 0);
    close(fd);
    if (rc != 0)
        return -1;
    if (abi < 3 && deny_path_truncate() != 0)
        return -1;
    return 0;
}

int sandbox_landlock_restrict_to_workspace(const char *workspace)
{
    return sandbox_landlock_restrict_for_abi(workspace, 0);
}

#endif /* __linux__ */
