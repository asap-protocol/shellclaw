/**
 * @file test_tweetnacl_smoke.c
 * @brief Compile/link smoke test for vendored TweetNaCl (task 5.2); not RFC vector tests (5.3).
 */

#include <stddef.h>
#include <stdint.h>

#include "tweetnacl.h"

void randombytes(unsigned char *x, unsigned long long n)
{
	unsigned long long i;

	for (i = 0; i < n; i++) {
		x[i] = (unsigned char)(i & 0xffU);
	}
}

int main(void)
{
	unsigned char pk[crypto_sign_ed25519_PUBLICKEYBYTES];
	unsigned char sk[crypto_sign_ed25519_SECRETKEYBYTES];
	const unsigned char msg[] = "shellclaw-tweetnacl-smoke";
	unsigned char sm[sizeof(msg) - 1U + crypto_sign_ed25519_BYTES];
	unsigned char opened[sizeof(msg) - 1U + crypto_sign_ed25519_BYTES];
	unsigned long long smlen;
	unsigned long long mlen;
	int rc;

	rc = crypto_sign_ed25519_keypair(pk, sk);
	if (rc != 0) {
		return 1;
	}

	smlen = 0;
	rc = crypto_sign_ed25519(sm, &smlen, msg, sizeof(msg) - 1U, sk);
	if (rc != 0 || smlen != sizeof(msg) - 1U + crypto_sign_ed25519_BYTES) {
		return 2;
	}

	mlen = 0;
	rc = crypto_sign_ed25519_open(opened, &mlen, sm, smlen, pk);
	if (rc != 0 || mlen != sizeof(msg) - 1U) {
		return 3;
	}

	return 0;
}
