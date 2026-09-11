/**
 * @file jcs.h
 * @brief JSON Canonicalization Scheme (RFC 8785) for deterministic signing.
 *
 * Subset sufficient for ShellClaw ASAP manifest trees (cJSON): objects with
 * lexicographically sorted UTF-8 keys, arrays, strings, booleans, null, and
 * integers in the IEEE-754 safe integer range. Non-finite numbers are rejected.
 */
#ifndef SHELLCLAW_CRYPTO_JCS_H
#define SHELLCLAW_CRYPTO_JCS_H

#include <stddef.h>

struct cJSON;
typedef struct cJSON cJSON;

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Serialize @p root to JCS canonical UTF-8 bytes.
 * Caller must free *out with free().
 *
 * @return 0 on success, -1 on NULL root, unsupported value, or OOM.
 */
int jcs_canonicalize(const cJSON *root, unsigned char **out, size_t *out_len);

#ifdef __cplusplus
}
#endif

#endif /* SHELLCLAW_CRYPTO_JCS_H */
