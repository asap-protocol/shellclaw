/**
 * @file rate_limit.h
 * @brief Gateway rate limiter: per-IP /asap.
 *
 * Fixed-size table with linear probing.  Thread-safe via internal mutex.
 */
#ifndef SHELLCLAW_GATEWAY_RATE_LIMIT_H
#define SHELLCLAW_GATEWAY_RATE_LIMIT_H

#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ASAP_RATE_LIMIT_RPM 10
#define ASAP_RATE_WINDOW_SECS 60

/**
 * Check whether @p ip has exceeded the /asap rate limit and record the request.
 *
 * @return 0 if allowed, 1 if limit exceeded.
 */
int rate_limit_asap(const char *ip, time_t now);

/** Reset all counters (unit tests). */
void rate_limit_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* SHELLCLAW_GATEWAY_RATE_LIMIT_H */
