/**
 * @file crypto.h
 * @brief Cryptographic helpers: OS randomness and Ed25519 sign/verify (TweetNaCl).
 */

#ifndef SHELLCLAW_CRYPTO_H
#define SHELLCLAW_CRYPTO_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Ed25519 public key size (bytes). */
#define CRYPTO_ED25519_PUBLIC_KEY_SIZE 32U
/** Ed25519 secret key size (TweetNaCl: 32-byte seed + 32-byte public key). */
#define CRYPTO_ED25519_PRIVATE_KEY_SIZE 64U
/** Ed25519 signature size (bytes). */
#define CRYPTO_ED25519_SIGNATURE_SIZE 64U

/**
 * Read @p len cryptographically strong random bytes from the OS (e.g. `/dev/urandom`).
 * @return 0 on success, -1 on invalid args or I/O failure.
 */
int crypto_read_urandom(void *buf, size_t len);

/**
 * Generate an Ed25519 keypair (32-byte public key, 64-byte secret key).
 * @return 0 on success, -1 on NULL outputs or RNG failure.
 */
int crypto_ed25519_keypair(uint8_t *pub_out, uint8_t *priv_out);

/**
 * Ed25519 sign: detached 64-byte signature for @p message.
 * @p private_key must be @ref CRYPTO_ED25519_PRIVATE_KEY_SIZE bytes (TweetNaCl secret key).
 * @return 0 on success, -1 on invalid args or signing failure.
 */
int crypto_ed25519_sign(const uint8_t *private_key, size_t private_key_len,
                        const uint8_t *message, size_t message_len,
                        uint8_t *signature_out, size_t signature_out_len);

/**
 * Ed25519 verify: detached signature over @p message.
 * @return 1 if valid, 0 if invalid, -1 on invalid args.
 */
int crypto_ed25519_verify(const uint8_t *public_key, size_t public_key_len,
                          const uint8_t *message, size_t message_len,
                          const uint8_t *signature, size_t signature_len);

/**
 * Pin the next @ref randombytes bytes used by TweetNaCl (tests only).
 * Cleared by @ref crypto_test_clear_randombytes_seed.
 */
void crypto_test_set_randombytes_seed(const uint8_t seed[32]);
void crypto_test_clear_randombytes_seed(void);

/**
 * Force @ref crypto_read_urandom to fail (tests only). Cleared by
 * @ref crypto_test_clear_force_urandom_fail.
 */
void crypto_test_force_urandom_fail(int enabled);
void crypto_test_clear_force_urandom_fail(void);

/**
 * Standard base64 encode (no line breaks). @p out must hold at least
 * 4 * ((in_len + 2) / 3) + 1 bytes.
 * @return encoded length on success, -1 on invalid args or buffer too small.
 */
int crypto_base64_encode(const uint8_t *in, size_t in_len, char *out, size_t out_cap);

/**
 * Standard base64 decode. Writes up to @p out_cap bytes to @p out.
 * Padding rules are relaxed for internal manifest use; do not feed untrusted
 * input without additional validation.
 * @return decoded length on success, -1 on invalid input or buffer too small.
 */
int crypto_base64_decode(const char *in, uint8_t *out, size_t out_cap);

#ifdef __cplusplus
}
#endif

#endif /* SHELLCLAW_CRYPTO_H */
