/**
 * @file auth.h
 * @brief Pairing code generation and bearer token validation for the gateway.
 */

#ifndef SHELLCLAW_GATEWAY_AUTH_H
#define SHELLCLAW_GATEWAY_AUTH_H

#include <stddef.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Opaque auth context. */
typedef struct auth_ctx auth_ctx_t;

/**
 * Initialize auth context with tokens file path.
 * Default path ~/.shellclaw/auth_tokens.json if tokens_path is NULL or empty.
 *
 * @param tokens_path Path to JSON tokens file; NULL for default.
 * @return Allocated auth_ctx_t; caller must auth_cleanup(). NULL on error.
 */
auth_ctx_t *auth_init(const char *tokens_path);

/**
 * Release auth context. Safe to call with NULL.
 *
 * @param ctx Auth context (may be NULL).
 */
void auth_cleanup(auth_ctx_t *ctx);

/**
 * Get or create pairing code. If no tokens file or empty, generate random 6-digit
 * code, print to stdout, and return allocated code string. Otherwise return NULL.
 *
 * @param ctx Auth context.
 * @return Allocated 6-digit code string; caller must free. NULL if tokens exist.
 */
char *auth_get_or_create_pairing_code(auth_ctx_t *ctx);

/** Buffer size for a pairing code and its terminator. */
#define PAIR_CODE_SIZE 7

/** Lifetime of a code for an additional device, in seconds. */
#define PAIR_DEVICE_CODE_SECS 300

/**
 * Issue a temporary code after a local operator authorizes a device.
 * Call on the gateway thread. Replace any pending code.
 *
 * @param ctx Auth context.
 * @param now Monotonic clock time in seconds.
 * @param code Buffer for the new code.
 * @return 0 on success, -1 if the code cannot be issued.
 */
int auth_issue_device_code(auth_ctx_t *ctx, time_t now, char code[PAIR_CODE_SIZE]);

/**
 * Pair with code: validate 6-digit code, add token to file, copy to token_out.
 *
 * @param ctx        Auth context.
 * @param code       6-digit pairing code.
 * @param token_out  Buffer to receive the new token.
 * @param token_size Size of token_out.
 * @return 0 on success, non-zero on invalid code or error.
 */
int auth_pair(auth_ctx_t *ctx, const char *code, char *token_out, size_t token_size);

/**
 * Validate bearer token against stored tokens.
 *
 * @param ctx   Auth context.
 * @param token Token to validate.
 * @return 1 if valid, 0 otherwise.
 */
int auth_validate_token(auth_ctx_t *ctx, const char *token);

/* --- Brute-force lockout for /pair (Task 6.4 / PRD CR-7) --- */

/** Maximum failed pairing attempts before an IP is locked out. */
#define PAIR_LOCKOUT_MAX_FAILS 5

/** Lockout duration in seconds after exceeding PAIR_LOCKOUT_MAX_FAILS. */
#define PAIR_LOCKOUT_WINDOW_SECS 300

/**
 * Check whether @p ip is currently locked out from attempting to pair.
 *
 * @param ctx  Auth context.
 * @param ip   Client IP string (NULL treated as "unknown").
 * @param now  Current time (injectable for tests; use time(NULL) in production).
 * @return     1 if the IP is locked out, 0 if pairing may proceed.
 */
int auth_pair_check_lockout(auth_ctx_t *ctx, const char *ip, time_t now);

/**
 * Record a failed pairing attempt for @p ip.
 * After PAIR_LOCKOUT_MAX_FAILS consecutive failures the IP is locked out
 * for PAIR_LOCKOUT_WINDOW_SECS seconds.
 *
 * @param ctx  Auth context.
 * @param ip   Client IP string (NULL treated as "unknown").
 * @param now  Current time.
 */
void auth_pair_record_failure(auth_ctx_t *ctx, const char *ip, time_t now);

/**
 * Clear the failure counter for @p ip (call on successful pairing).
 *
 * @param ctx  Auth context.
 * @param ip   Client IP string (NULL treated as "unknown").
 */
void auth_pair_clear_ip(auth_ctx_t *ctx, const char *ip);

#ifdef __cplusplus
}
#endif

#endif /* SHELLCLAW_GATEWAY_AUTH_H */
