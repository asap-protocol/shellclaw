/**
 * @file manifest_sign.h
 * @brief ASAP SignedManifest JSON (JCS + Ed25519).
 */
#ifndef SHELLCLAW_ASAP_MANIFEST_SIGN_H
#define SHELLCLAW_ASAP_MANIFEST_SIGN_H

struct config;
typedef struct config config_t;

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Build SignedManifest JSON. Requires @ref manifest_keys_ensure_loaded first.
 *
 * @return Allocated JSON string, or NULL on error.
 */
char *manifest_build_signed_json(const config_t *cfg);

#ifdef __cplusplus
}
#endif

#endif /* SHELLCLAW_ASAP_MANIFEST_SIGN_H */
