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
 * Use a private read-only mount guard for commands on Landlock ABI 1 or 2.
 * @param workspace Workspace directory, or NULL/empty to skip.
 * @return          0 on success or skip; -1 if the ruleset cannot apply.
 *
 * Example: sandbox_landlock_restrict_to_workspace("/home/user/.shellclaw");
 */
int sandbox_landlock_restrict_to_workspace(const char *workspace);

/**
 * Build and discard the workspace ruleset without restrict_self.
 * Same skip/error contract as restrict. Used so tests can cover the
 * builder without locking the process (gcov cannot write after restrict).
 *
 * Example: sandbox_landlock_prepare("/tmp/ws");
 */
int sandbox_landlock_prepare(const char *workspace);

#ifdef __linux__
/**
 * Check whether the kernel needs a mount guard to restrict file truncation.
 * @return 1 for ABI 1 or 2, 0 for ABI 3 or later, -1 if Landlock is unavailable.
 */
int sandbox_landlock_needs_mount_guard(void);
#endif

#ifdef __cplusplus
}
#endif

#endif /* SHELLCLAW_SANDBOX_LANDLOCK_H */
