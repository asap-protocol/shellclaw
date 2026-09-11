/**
 * @file cron.h
 * @brief Cron scheduler: parse schedules, next_run, trigger injection via cron channel.
 */

#ifndef SHELLCLAW_CRON_H
#define SHELLCLAW_CRON_H

#include "channels/channel.h"
#include "tools/tool.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Parse schedule and compute next_run strictly after `now`.
 * Formats: "cron:min hour dom month dow", "interval:N", "at:unix_ts".
 * Cron: 5 fields, * or N or N-M. dow 0-6 (Sun-Sat).
 * Cron expressions start searching at the next minute boundary so a just-fired
 * job cannot remain due in the same minute.
 *
 * @param schedule Schedule string.
 * @param now      Current Unix timestamp.
 * @param next_out Output: next run time (always > now on success for cron:/interval:).
 * @return 0 on success, -1 on parse error.
 */
int cron_parse_next_run(const char *schedule, long long now, long long *next_out);

/**
 * Check if schedule is one-shot (at:ts). One-shot jobs are deleted after run.
 *
 * @param schedule Schedule string.
 * @return 1 if one-shot, 0 otherwise.
 */
int cron_is_one_shot(const char *schedule);

/**
 * Get the cron channel (poll returns due jobs, send routes to target channel).
 */
const channel_t *channel_cron_get(void);

/** Get the cron tool for agent (list, create, delete, toggle jobs). */
const tool_t *tool_cron_get(void);

/**
 * Create a cron job (shared by tool and HTTP handler).
 * If id_out[0] == '\0', a random ID is generated.
 *
 * @param schedule  Schedule string (interval:N, at:ts, or cron expression).
 * @param message   Message to inject when job fires.
 * @param channel   Target channel name (e.g., "cli"). NULL defaults to "cli".
 * @param recipient Recipient ID. NULL defaults to "default".
 * @param id_out    Buffer for job ID. If non-empty on entry, used as-is; else generated.
 * @param id_size   Size of id_out buffer.
 * @return 0 on success, -1 on error.
 */
int cron_create_job(const char *schedule, const char *message,
                    const char *channel, const char *recipient,
                    char *id_out, size_t id_size);

#ifdef __cplusplus
}
#endif

#endif /* SHELLCLAW_CRON_H */
