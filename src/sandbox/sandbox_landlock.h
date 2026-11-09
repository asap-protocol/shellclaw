/**
 * @file sandbox_landlock.h
 * @brief Landlock workspace filesystem bound for sandbox_exec (Linux).
 *
 * Non-Linux builds compile a stub that returns 0. On Linux, failure is
 * fail-closed: the caller must not exec on the host tree.
 */
#ifndef SHELLCLAW_SANDBOX_LANDLOCK_H
#define SHELLCLAW_SANDBOX_LANDLOCK_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Restrict the calling thread to @p workspace (RW) plus a small RO/exec set
 * needed by /bin/sh. Empty @p workspace skips Landlock (returns 0).
 *
 * On Landlock ABI 1 and 2 (JetPack 6.2 / kernel 5.15 is ABI 1) the kernel
 * cannot gate truncate(2). This call then also denies that path syscall.
 * ftruncate(2) stays allowed because it needs a writable descriptor, and
 * WRITE_FILE already rejects opening a host file.
 *
 * @param workspace Absolute workspace directory, or NULL/empty to skip.
 * @return          0 on success or skip; -1 if the ruleset cannot apply.
 *
 * Example: sandbox_landlock_restrict_to_workspace("/home/user/.shellclaw");
 */
int sandbox_landlock_restrict_to_workspace(const char *workspace);

/**
 * Same bound as sandbox_landlock_restrict_to_workspace(), built as if the
 * kernel Landlock ABI were @p abi. Pass 0 to probe the running kernel.
 * An ABI below 3 installs the path-truncate denial even on a newer kernel.
 *
 * @param workspace Absolute workspace directory, or NULL/empty to skip.
 * @param abi       Landlock ABI to emulate, or 0 to probe.
 * @return          0 on success or skip; -1 if the ruleset cannot apply.
 *
 * Example: sandbox_landlock_restrict_for_abi("/tmp/ws", 1);
 */
int sandbox_landlock_restrict_for_abi(const char *workspace, int abi);

/**
 * Build and discard the workspace ruleset without restrict_self.
 * Same skip/error contract as restrict. Used so tests can cover the
 * builder without locking the process (gcov cannot write after restrict).
 *
 * Example: sandbox_landlock_prepare("/tmp/ws");
 */
int sandbox_landlock_prepare(const char *workspace);

#ifdef __cplusplus
}
#endif

#endif /* SHELLCLAW_SANDBOX_LANDLOCK_H */
