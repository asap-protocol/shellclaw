/**
 * @file manifest_keys.h
 * @brief Ed25519 signing keys for ASAP SignedManifest (load, rotate, test hooks).
 */
#ifndef SHELLCLAW_ASAP_MANIFEST_KEYS_H
#define SHELLCLAW_ASAP_MANIFEST_KEYS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Load or create Ed25519 keypair under $SHELLCLAW_HOME/keys/ (default ~/.shellclaw/keys/).
 * Creates ed25519.priv (64 bytes) and ed25519.pub (32 bytes) with mode 0600 on first use.
 * Refuses if ed25519.priv exists with group/other permission bits set.
 */
int manifest_keys_load(char *err, size_t err_len);

/**
 * Ensure signing keys are loaded (idempotent). Use before building SignedManifest.
 *
 * @return 0 on success, -1 on I/O, permission, or crypto error.
 */
int manifest_keys_ensure_loaded(char *err, size_t err_len);

/** Return loaded public key (32 bytes) after load succeeds. */
const uint8_t *manifest_keys_public(void);

/** Return loaded secret key (64 bytes) after load succeeds. */
const uint8_t *manifest_keys_private(void);

/** Tests only: use @p keys_dir for ed25519.{priv,pub} (NULL restores default layout). */
void manifest_keys_set_dir_for_test(const char *keys_dir);

/**
 * Rotate Ed25519 keypair on disk with atomic backup of prior keys.
 *
 * @return 0 on success, -1 on failure.
 */
int manifest_keys_rotate(char *err, size_t err_len);

/** Clear in-memory keys so load runs again. */
void manifest_keys_reset(void);

/** Tests only: next atomic write to ed25519.pub fails once (cleared after use). */
void manifest_keys_test_set_fail_pub_write(int enabled);

/** Tests only: next write to a .bak.* path fails once (cleared after use). */
void manifest_keys_test_set_fail_backup_write(int enabled);

#ifdef __cplusplus
}
#endif

#endif /* SHELLCLAW_ASAP_MANIFEST_KEYS_H */
