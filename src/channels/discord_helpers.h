/**
 * @file discord_helpers.h
 * @brief Pure helpers shared by Discord channel and unit tests (allowlist, session id, mentions,
 *        REST 429 backoff calculation, Gateway RX append).
 */

#ifndef SHELLCLAW_DISCORD_HELPERS_H
#define SHELLCLAW_DISCORD_HELPERS_H

#include <stddef.h>

#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Match one allowlist row against author id (exact string compare). */
int discord_helpers_allow_entry_equals(const char *allow_entry, const char *author_id);

/** True if @p author_id equals any entry in @p allowed (length @p count). */
int discord_helpers_user_in_allowlist(const char *const *allowed, int count, const char *author_id);

/**
 * Build session id `discord:c:<channel_id>` into @p out.
 * @return 0 on success, -1 if inputs invalid or buffer too small.
 */
int discord_helpers_session_id_from_channel(const char *channel_id, char *out, size_t outsiz);

/** True if @p mentions JSON array contains an object whose id equals @p bot_id. */
int discord_helpers_mentions_include_bot(const cJSON *mentions, const char *bot_id);

/**
 * Sleep duration before retry after Discord REST 429 or transport retry (matches discord_send policy).
 * @param attempt           Zero-based attempt index (used when retry_after_sec <= 0).
 * @param retry_after_sec   From Retry-After header or JSON; <= 0 selects exponential path.
 * @param jitter_raw        Raw jitter (e.g. from discord_jitter_ms); Retry-After path uses jitter % 50.
 */
int discord_helpers_send_backoff_ms(int attempt, double retry_after_sec, int jitter_raw,
                                    int base_ms, int cap_ms);

/**
 * Route a MESSAGE_CREATE payload: ignore bots, enforce allowlist, guild mention-only.
 * @param payload         MESSAGE_CREATE `d` object (cJSON).
 * @param allowed         Allowlisted author ids.
 * @param allowed_count   Length of @p allowed.
 * @param bot_user_id     Bot user id for guild mention checks (may be NULL in guild → reject).
 * @param session_out     Output buffer for `discord:c:<channel_id>` when accepted.
 * @param session_outsz   Size of @p session_out.
 * @return 1 if message should be handled, 0 if ignored, -1 on invalid args.
 */
int discord_helpers_route_message_create(const cJSON *payload, const char *const *allowed,
                                         int allowed_count, const char *bot_user_id,
                                         char *session_out, size_t session_outsz);

/**
 * Append one Gateway RX fragment, growing so payload plus a trailing NUL always fit.
 *
 * @return 0 on success, -1 if the total would exceed @p max_cap or allocation failed.
 *
 * Example: two 64 KiB LWS fragments must grow past 131072 so the NUL fits.
 */
int discord_helpers_rx_append(char **buf, size_t *len, size_t *cap, const void *in,
                              size_t chunk_len, size_t max_cap);

#ifdef __cplusplus
}
#endif

#endif /* SHELLCLAW_DISCORD_HELPERS_H */
