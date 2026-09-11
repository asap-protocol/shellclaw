/**
 * @file test_crypto.c
 * @brief crypto_read_urandom, Ed25519 (TweetNaCl), RFC 8032 vectors, tamper detection.
 */
#define _POSIX_C_SOURCE 200809L

#include "crypto/crypto.h"
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

void randombytes(unsigned char *x, unsigned long long n);

#define ASSERT(c) do { if (!(c)) { fprintf(stderr, "FAIL: %s:%d %s\n", __FILE__, __LINE__, #c); return 1; } } while (0)
#define RUN(t) do { int _r = (t); if (_r) return _r; } while (0)

static int hex_nibble(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return 10 + (c - 'a');
	if (c >= 'A' && c <= 'F')
		return 10 + (c - 'A');
	return -1;
}

static int hex_decode(uint8_t *out, size_t out_len, const char *hex)
{
	size_t i;
	if (strlen(hex) != out_len * 2U)
		return -1;
	for (i = 0; i < out_len; i++) {
		int hi;
		int lo;
		hi = hex_nibble(hex[i * 2U]);
		lo = hex_nibble(hex[i * 2U + 1U]);
		if (hi < 0 || lo < 0)
			return -1;
		out[i] = (uint8_t)((hi << 4) | lo);
	}
	return 0;
}

static int test_read_urandom(void)
{
	uint8_t a[16];
	uint8_t b[16];
	ASSERT(crypto_read_urandom(a, sizeof(a)) == 0);
	ASSERT(crypto_read_urandom(b, sizeof(b)) == 0);
	ASSERT(memcmp(a, b, sizeof(a)) != 0);
	ASSERT(crypto_read_urandom(NULL, 4) == -1);
	ASSERT(crypto_read_urandom(a, 0) == 0);
	return 0;
}

static int test_ed25519_sign_verify_roundtrip(void)
{
	uint8_t pk[CRYPTO_ED25519_PUBLIC_KEY_SIZE];
	uint8_t sk[CRYPTO_ED25519_PRIVATE_KEY_SIZE];
	uint8_t sig[CRYPTO_ED25519_SIGNATURE_SIZE];
	const uint8_t msg[] = "shellclaw phase5";
	ASSERT(crypto_ed25519_keypair(pk, sk) == 0);
	ASSERT(crypto_ed25519_sign(sk, sizeof(sk), msg, sizeof(msg) - 1U, sig, sizeof(sig)) == 0);
	ASSERT(crypto_ed25519_verify(pk, sizeof(pk), msg, sizeof(msg) - 1U, sig, sizeof(sig)) == 1);
	sig[0] ^= 0xFFU;
	ASSERT(crypto_ed25519_verify(pk, sizeof(pk), msg, sizeof(msg) - 1U, sig, sizeof(sig)) == 0);
	return 0;
}

static int test_ed25519_tamper_message(void)
{
	uint8_t pk[CRYPTO_ED25519_PUBLIC_KEY_SIZE];
	uint8_t sk[CRYPTO_ED25519_PRIVATE_KEY_SIZE];
	uint8_t sig[CRYPTO_ED25519_SIGNATURE_SIZE];
	uint8_t msg[] = "tamper-message";
	ASSERT(crypto_ed25519_keypair(pk, sk) == 0);
	ASSERT(crypto_ed25519_sign(sk, sizeof(sk), msg, sizeof(msg) - 1U, sig, sizeof(sig)) == 0);
	msg[0] ^= 0x01U;
	ASSERT(crypto_ed25519_verify(pk, sizeof(pk), msg, sizeof(msg) - 1U, sig, sizeof(sig)) == 0);
	return 0;
}

static int test_ed25519_keypair_deterministic_seed(void)
{
	static const uint8_t seed[32] = {
		0x42U, 0x11U, 0x51U, 0xa4U, 0x59U, 0xfaU, 0xeaU, 0xdeU,
		0x3dU, 0x24U, 0x71U, 0x15U, 0xf9U, 0x4aU, 0xedU, 0xaeU,
		0x42U, 0x31U, 0x81U, 0x24U, 0x09U, 0x5aU, 0xfaU, 0xbeU,
		0x4dU, 0x14U, 0x51U, 0xa5U, 0x59U, 0xfaU, 0xedU, 0xeeU
	};
	uint8_t pk1[CRYPTO_ED25519_PUBLIC_KEY_SIZE];
	uint8_t sk1[CRYPTO_ED25519_PRIVATE_KEY_SIZE];
	uint8_t pk2[CRYPTO_ED25519_PUBLIC_KEY_SIZE];
	uint8_t sk2[CRYPTO_ED25519_PRIVATE_KEY_SIZE];
	crypto_test_set_randombytes_seed(seed);
	ASSERT(crypto_ed25519_keypair(pk1, sk1) == 0);
	crypto_test_set_randombytes_seed(seed);
	ASSERT(crypto_ed25519_keypair(pk2, sk2) == 0);
	ASSERT(memcmp(pk1, pk2, sizeof(pk1)) == 0);
	ASSERT(memcmp(sk1, sk2, sizeof(sk1)) == 0);
	crypto_test_clear_randombytes_seed();
	return 0;
}

/* RFC 8032 section 7.1 TEST 1 (empty message). */
static int test_ed25519_rfc8032_vector1(void)
{
	uint8_t pk[CRYPTO_ED25519_PUBLIC_KEY_SIZE];
	uint8_t sig[CRYPTO_ED25519_SIGNATURE_SIZE];
	ASSERT(hex_decode(pk, sizeof(pk),
	                  "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a") == 0);
	ASSERT(hex_decode(sig, sizeof(sig),
	                  "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e06522490155"
	                  "5fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b") == 0);
	ASSERT(crypto_ed25519_verify(pk, sizeof(pk), NULL, 0U, sig, sizeof(sig)) == 1);
	return 0;
}

/* RFC 8032 section 7.1 TEST 2 (message 0x72). */
static int test_ed25519_rfc8032_vector2(void)
{
	uint8_t pk[CRYPTO_ED25519_PUBLIC_KEY_SIZE];
	uint8_t sig[CRYPTO_ED25519_SIGNATURE_SIZE];
	uint8_t msg[1];
	ASSERT(hex_decode(pk, sizeof(pk),
	                  "3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c") == 0);
	ASSERT(hex_decode(sig, sizeof(sig),
	                  "92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da"
	                  "085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00") == 0);
	msg[0] = 0x72U;
	ASSERT(crypto_ed25519_verify(pk, sizeof(pk), msg, sizeof(msg), sig, sizeof(sig)) == 1);
	return 0;
}

static int test_ed25519_keypair_fails_without_rng(void)
{
	uint8_t pk[CRYPTO_ED25519_PUBLIC_KEY_SIZE];
	uint8_t sk[CRYPTO_ED25519_PRIVATE_KEY_SIZE];
	crypto_test_force_urandom_fail(1);
	ASSERT(crypto_ed25519_keypair(pk, sk) != 0);
	crypto_test_clear_force_urandom_fail();
	return 0;
}

static int test_randombytes_aborts_on_urandom_failure(void)
{
	pid_t pid;
	int status;

	pid = fork();
	ASSERT(pid >= 0);
	if (pid == 0) {
		unsigned char buf[8];
		crypto_test_force_urandom_fail(1);
		randombytes(buf, sizeof(buf));
		_exit(0);
	}
	ASSERT(waitpid(pid, &status, 0) == pid);
	ASSERT(WIFSIGNALED(status));
	ASSERT(WTERMSIG(status) == SIGABRT);
	return 0;
}

static int test_ed25519_empty_message_sign(void)
{
	uint8_t pk[CRYPTO_ED25519_PUBLIC_KEY_SIZE];
	uint8_t sk[CRYPTO_ED25519_PRIVATE_KEY_SIZE];
	uint8_t sig[CRYPTO_ED25519_SIGNATURE_SIZE];
	ASSERT(crypto_ed25519_keypair(pk, sk) == 0);
	ASSERT(crypto_ed25519_sign(sk, sizeof(sk), NULL, 0U, sig, sizeof(sig)) == 0);
	ASSERT(crypto_ed25519_verify(pk, sizeof(pk), NULL, 0U, sig, sizeof(sig)) == 1);
	return 0;
}

/* Exercises the zero-length + NULL pointer verify path (the M0 memcmp guard):
 * sign an empty message and verify with (NULL, 0). Deterministic seed keeps
 * the assertion reproducible and avoids /dev/urandom dependencies. */
static int test_ed25519_verify_empty_message_null_pointer(void)
{
	static const uint8_t seed[32] = {
		0x9aU, 0x12U, 0x7cU, 0xe4U, 0x55U, 0xabU, 0x01U, 0xf3U,
		0x6dU, 0x8eU, 0xc2U, 0x37U, 0xb0U, 0x41U, 0x9dU, 0xa6U,
		0xeeU, 0x30U, 0x5bU, 0x14U, 0x21U, 0x77U, 0xf9U, 0x88U,
		0x04U, 0x6aU, 0x3bU, 0xd1U, 0xc0U, 0x52U, 0x2eU, 0x7fU
	};
	uint8_t pk[CRYPTO_ED25519_PUBLIC_KEY_SIZE];
	uint8_t sk[CRYPTO_ED25519_PRIVATE_KEY_SIZE];
	uint8_t sig[CRYPTO_ED25519_SIGNATURE_SIZE];
	crypto_test_set_randombytes_seed(seed);
	ASSERT(crypto_ed25519_keypair(pk, sk) == 0);
	ASSERT(crypto_ed25519_sign(sk, sizeof(sk), NULL, 0U, sig, sizeof(sig)) == 0);
	ASSERT(crypto_ed25519_verify(pk, sizeof(pk), NULL, 0U, sig, sizeof(sig)) == 1);
	crypto_test_clear_randombytes_seed();
	return 0;
}

static int test_base64_roundtrip_and_rejects(void)
{
	const uint8_t raw[] = { 0x00U, 0xffU, 0x10U, 0x20U };
	char enc[32];
	uint8_t dec[8];
	int n;

	ASSERT(crypto_base64_encode(raw, sizeof(raw), enc, sizeof(enc)) > 0);
	ASSERT(strcmp(enc, "AP8QIA==") == 0);
	n = crypto_base64_decode(enc, dec, sizeof(dec));
	ASSERT(n == (int)sizeof(raw));
	ASSERT(memcmp(dec, raw, sizeof(raw)) == 0);
	ASSERT(crypto_base64_encode(NULL, 1U, enc, sizeof(enc)) == -1);
	ASSERT(crypto_base64_encode(raw, sizeof(raw), NULL, sizeof(enc)) == -1);
	ASSERT(crypto_base64_encode(raw, sizeof(raw), enc, 4U) == -1);
	ASSERT(crypto_base64_decode("AP8", dec, sizeof(dec)) == -1);
	ASSERT(crypto_base64_decode("AP8!IA==", dec, sizeof(dec)) == -1);
	ASSERT(crypto_base64_decode(NULL, dec, sizeof(dec)) == -1);
	return 0;
}

static int test_ed25519_sign_null_args(void)
{
	uint8_t pk[CRYPTO_ED25519_PUBLIC_KEY_SIZE];
	uint8_t sk[CRYPTO_ED25519_PRIVATE_KEY_SIZE];
	uint8_t sig[CRYPTO_ED25519_SIGNATURE_SIZE];
	const uint8_t msg[] = "test";
	uint8_t small[4];

	ASSERT(crypto_ed25519_keypair(pk, sk) == 0);
	ASSERT(crypto_ed25519_sign(NULL, sizeof(sk), msg, sizeof(msg) - 1U, sig, sizeof(sig)) == -1);
	ASSERT(crypto_ed25519_sign(sk, 0, msg, sizeof(msg) - 1U, sig, sizeof(sig)) == -1);
	ASSERT(crypto_ed25519_sign(sk, sizeof(sk), NULL, 1U, sig, sizeof(sig)) == -1);
	ASSERT(crypto_ed25519_sign(sk, sizeof(sk), msg, sizeof(msg) - 1U, NULL, sizeof(sig)) == -1);
	ASSERT(crypto_ed25519_sign(sk, sizeof(sk), msg, sizeof(msg) - 1U, small, sizeof(small)) == -1);
	return 0;
}

static int test_ed25519_verify_null_args(void)
{
	uint8_t pk[CRYPTO_ED25519_PUBLIC_KEY_SIZE];
	uint8_t sk[CRYPTO_ED25519_PRIVATE_KEY_SIZE];
	uint8_t sig[CRYPTO_ED25519_SIGNATURE_SIZE];
	const uint8_t msg[] = "test";

	ASSERT(crypto_ed25519_keypair(pk, sk) == 0);
	ASSERT(crypto_ed25519_sign(sk, sizeof(sk), msg, sizeof(msg) - 1U, sig, sizeof(sig)) == 0);
	ASSERT(crypto_ed25519_verify(NULL, sizeof(pk), msg, sizeof(msg) - 1U, sig, sizeof(sig)) == -1);
	ASSERT(crypto_ed25519_verify(pk, sizeof(pk), NULL, 1U, sig, sizeof(sig)) == -1);
	ASSERT(crypto_ed25519_verify(pk, sizeof(pk), msg, sizeof(msg) - 1U, NULL, sizeof(sig)) == -1);
	return 0;
}

int main(void)
{
	RUN(test_read_urandom());
	RUN(test_ed25519_sign_verify_roundtrip());
	RUN(test_ed25519_tamper_message());
	RUN(test_ed25519_keypair_deterministic_seed());
	RUN(test_ed25519_rfc8032_vector1());
	RUN(test_ed25519_rfc8032_vector2());
	RUN(test_ed25519_keypair_fails_without_rng());
	RUN(test_randombytes_aborts_on_urandom_failure());
	RUN(test_ed25519_empty_message_sign());
	RUN(test_ed25519_verify_empty_message_null_pointer());
	RUN(test_base64_roundtrip_and_rejects());
	RUN(test_ed25519_sign_null_args());
	RUN(test_ed25519_verify_null_args());
	printf("test_crypto: all tests passed\n");
	return 0;
}
