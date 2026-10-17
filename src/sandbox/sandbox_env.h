/**
 * @file sandbox_env.h
 * @brief Drop host secrets before exec of a sandboxed shell command.
 */
#ifndef SHELLCLAW_SANDBOX_ENV_H
#define SHELLCLAW_SANDBOX_ENV_H

/**
 * Replace the process environment with PATH, locale, and an optional HOME.
 *
 * Call only in the child that is about to execl sh -c. API keys, bot
 * tokens, and LD_* variables from the agent are not passed through.
 *
 * Example: sandbox_scrub_command_environment("/home/me/.shellclaw/workspace");
 */
void sandbox_scrub_command_environment(const char *workspace);

#endif
