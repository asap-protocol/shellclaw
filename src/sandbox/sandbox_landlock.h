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
 * @param workspace Absolute workspace directory, or NULL/empty to skip.
 * @return          0 on success or skip; -1 if the ruleset cannot apply.
 *
 * Example: sandbox_landlock_restrict_to_workspace("/home/user/.shellclaw");
 */
int sandbox_landlock_restrict_to_workspace(const char *workspace);

#ifdef __cplusplus
}
#endif

#endif /* SHELLCLAW_SANDBOX_LANDLOCK_H */
