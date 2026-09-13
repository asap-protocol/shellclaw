/**
 * @file crypto.c
 * @brief OS randomness and Ed25519 sign/verify via vendored TweetNaCl.
 */
#define _POSIX_C_SOURCE 200809L

#include "crypto/crypto.h"
#include "tweetnacl.h"

#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static uint8_t g_test_randombytes_seed[32];
static int g_test_randombytes_seed_set;
static int g_test_force_urandom_fail;

void randombytes(unsigned char *x, unsigned long long n)
{
	size_t len;
	if (g_test_randombytes_seed_set != 0 && n > 0U) {
		len = (size_t)n;
		if (len > sizeof(g_test_randombytes_seed))
			len = sizeof(g_test_randombytes_seed);
		memcpy(x, g_test_randombytes_seed, len);
		if (n > (unsigned long long)len)
			memset(x + len, 0, (size_t)n - len);
		return;
	}
	len = (size_t)n;
	if (len == 0U)
		return;
	if (crypto_read_urandom(x, len) != 0) {
		memset(x, 0, len);
		abort();
	}
}

void crypto_test_set_randombytes_seed(const uint8_t seed[32])
{
	if (!seed)
		return;
	memcpy(g_test_randombytes_seed, seed, sizeof(g_test_randombytes_seed));
	g_test_randombytes_seed_set = 1;
}

void crypto_test_clear_randombytes_seed(void)
{
	g_test_randombytes_seed_set = 0;
	memset(g_test_randombytes_seed, 0, sizeof(g_test_randombytes_seed));
}

void crypto_test_force_urandom_fail(int enabled)
{
	g_test_force_urandom_fail = enabled ? 1 : 0;
}

void crypto_test_clear_force_urandom_fail(void)
{
	g_test_force_urandom_fail = 0;
}

int crypto_read_urandom(void *buf, size_t len)
{
	int fd;
	uint8_t *out;
	size_t off;
	if (!buf && len > 0)
		return -1;
	if (len == 0)
		return 0;
	if (g_test_force_urandom_fail != 0)
		return -1;
	out = (uint8_t *)buf;
	fd = open("/dev/urandom", O_RDONLY);
	if (fd < 0)
		return -1;
	off = 0;
	while (off < len) {
		ssize_t n;
		n = read(fd, out + off, len - off);
		if (n <= 0) {
			close(fd);
			return -1;
		}
		off += (size_t)n;
	}
	close(fd);
	return 0;
}

int crypto_ed25519_keypair(uint8_t *pub_out, uint8_t *priv_out)
{
	if (!pub_out || !priv_out)
		return -1;
	if (g_test_randombytes_seed_set == 0) {
		uint8_t probe[32];
		if (crypto_read_urandom(probe, sizeof(probe)) != 0)
			return -1;
	}
	if (crypto_sign_ed25519_keypair(pub_out, priv_out) != 0)
		return -1;
	return 0;
}

int crypto_ed25519_sign(const uint8_t *private_key, size_t private_key_len,
                        const uint8_t *message, size_t message_len,
                        uint8_t *signature_out, size_t signature_out_len)
{
	unsigned char *sm;
	unsigned long long smlen;
	if (!private_key || private_key_len < crypto_sign_ed25519_SECRETKEYBYTES ||
	    (!message && message_len > 0U) || !signature_out ||
	    signature_out_len < CRYPTO_ED25519_SIGNATURE_SIZE)
		return -1;
	if (message_len > 0U && message_len > (size_t)(SIZE_MAX - crypto_sign_ed25519_BYTES))
		return -1;
	sm = (unsigned char *)malloc((size_t)message_len + crypto_sign_ed25519_BYTES);
	if (!sm)
		return -1;
	smlen = 0;
	if (crypto_sign_ed25519(sm, &smlen,
	                        message_len > 0U ? message : (const uint8_t *)"",
	                        (unsigned long long)message_len,
	                        private_key) != 0) {
		free(sm);
		return -1;
	}
	if (smlen != (unsigned long long)message_len + crypto_sign_ed25519_BYTES) {
		free(sm);
		return -1;
	}
	memcpy(signature_out, sm, CRYPTO_ED25519_SIGNATURE_SIZE);
	free(sm);
	return 0;
}

int crypto_ed25519_verify(const uint8_t *public_key, size_t public_key_len,
                          const uint8_t *message, size_t message_len,
                          const uint8_t *signature, size_t signature_len)
{
	unsigned char *sm;
	unsigned char *opened;
	unsigned long long smlen;
	unsigned long long mlen;
	int rc;
	int content_match;
	if (!public_key || public_key_len < crypto_sign_ed25519_PUBLICKEYBYTES ||
	    (!message && message_len > 0U) || !signature ||
	    signature_len < CRYPTO_ED25519_SIGNATURE_SIZE)
		return -1;
	if (message_len > 0U && message_len > (size_t)(SIZE_MAX - crypto_sign_ed25519_BYTES))
		return -1;
	smlen = (unsigned long long)message_len + crypto_sign_ed25519_BYTES;
	sm = (unsigned char *)malloc((size_t)smlen);
	if (!sm)
		return -1;
	memcpy(sm, signature, CRYPTO_ED25519_SIGNATURE_SIZE);
	if (message_len > 0U)
		memcpy(sm + crypto_sign_ed25519_BYTES, message, message_len);
	opened = (unsigned char *)malloc((size_t)message_len + crypto_sign_ed25519_BYTES);
	if (!opened) {
		free(sm);
		return -1;
	}
	mlen = 0;
	rc = crypto_sign_ed25519_open(opened, &mlen, sm, smlen, public_key);
	free(sm);
	/* memcmp with n==0 and a NULL pointer is UB per a strict reading of C;
	 * skip it for empty messages (NULL,0 is a valid verify input per the
	 * line-149 guard). */
	content_match = 1;
	if (message_len > 0U)
		content_match = (memcmp(opened, message, message_len) == 0);
	if (rc != 0 || mlen != (unsigned long long)message_len || !content_match) {
		free(opened);
		return 0;
	}
	free(opened);
	return 1;
}

static const char B64_TABLE[] =
	"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static int b64_decode_char(int c)
{
	if (c >= 'A' && c <= 'Z')
		return c - 'A';
	if (c >= 'a' && c <= 'z')
		return c - 'a' + 26;
	if (c >= '0' && c <= '9')
		return c - '0' + 52;
	if (c == '+')
		return 62;
	if (c == '/')
		return 63;
	return -1;
}

int crypto_base64_encode(const uint8_t *in, size_t in_len, char *out, size_t out_cap)
{
	size_t i;
	size_t o;
	size_t need;
	if (!in || !out)
		return -1;
	need = 4U * ((in_len + 2U) / 3U) + 1U;
	if (out_cap < need)
		return -1;
	o = 0;
	for (i = 0; i < in_len; i += 3U) {
		uint32_t v;
		int pad;
		v = (uint32_t)in[i] << 16;
		pad = 2;
		if (i + 1U < in_len) {
			v |= (uint32_t)in[i + 1U] << 8;
			pad = 1;
		} else {
			v &= 0xff0000U;
		}
		if (i + 2U < in_len) {
			v |= (uint32_t)in[i + 2U];
			pad = 0;
		} else {
			v &= 0xffff00U;
		}
		out[o++] = B64_TABLE[(v >> 18) & 63U];
		out[o++] = B64_TABLE[(v >> 12) & 63U];
		out[o++] = (pad < 2) ? B64_TABLE[(v >> 6) & 63U] : '=';
		out[o++] = (pad < 1) ? B64_TABLE[v & 63U] : '=';
	}
	out[o] = '\0';
	return (int)o;
}

int crypto_base64_decode(const char *in, uint8_t *out, size_t out_cap)
{
	size_t in_len;
	size_t i;
	size_t o;
	uint32_t acc;
	int bits;
	if (!in || !out)
		return -1;
	in_len = strlen(in);
	if (in_len == 0U || (in_len % 4U) != 0U)
		return -1;
	acc = 0;
	bits = 0;
	o = 0;
	for (i = 0; i < in_len; i++) {
		unsigned char c;
		int v;
		c = (unsigned char)in[i];
		if (c == '=')
			break;
		v = b64_decode_char((int)c);
		if (v < 0)
			return -1;
		acc = (acc << 6) | (uint32_t)v;
		bits += 6;
		if (bits >= 8) {
			bits -= 8;
			if (o >= out_cap)
				return -1;
			out[o++] = (uint8_t)((acc >> (unsigned)bits) & 0xffU);
		}
	}
	return (int)o;
}
