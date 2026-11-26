/**
 * @file heartbeat.h
 * @brief Heartbeat channel: periodic agent tick for autonomous activity.
 */
#ifndef SHELLCLAW_HEARTBEAT_H
#define SHELLCLAW_HEARTBEAT_H

#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

struct config;
typedef struct config config_t;

struct channel;
typedef struct channel channel_t;

/** Get the heartbeat channel (poll returns tick when interval elapsed, send routes to default or log). */
const channel_t *channel_heartbeat_get(void);

/** Return the configured log or CLI delivery channel for cron, or NULL.
 * Call with the agent lock held to serialize access with configuration reload.
 */
const channel_t *heartbeat_local_delivery(void);

/** After config reload without channel re-init (SIGHUP); updates live pointers only. */
void heartbeat_set_live_config(const config_t *cfg);

#ifdef SHELLCLAW_TEST
/** Override monotonic wall clock for unit tests. */
void heartbeat_test_set_now(time_t now);
void heartbeat_test_reset(void);
#endif

#ifdef __cplusplus
}
#endif

#endif /* SHELLCLAW_HEARTBEAT_H */
