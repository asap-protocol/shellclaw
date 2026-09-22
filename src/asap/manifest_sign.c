/**
 * @file manifest_sign.c
 * @brief ASAP SignedManifest JSON (JCS canonicalization + Ed25519).
 */
#define _POSIX_C_SOURCE 200809L

#include "asap/manifest_sign.h"
#include "asap/manifest_build.h"
#include "asap/manifest_keys.h"
#include "crypto/crypto.h"
#include "crypto/jcs.h"
#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MANIFEST_SIG_ALG "ed25519"
#define MANIFEST_TRUST_SELF_SIGNED "self-signed"
#define MANIFEST_B64_SIG_MAX 96
#define MANIFEST_B64_PUB_MAX 48

char *manifest_build_signed_json(const config_t *cfg)
{
	cJSON *manifest;
	cJSON *wrapper;
	cJSON *sig_block;
	unsigned char *canonical;
	size_t canon_len;
	uint8_t sig_raw[CRYPTO_ED25519_SIGNATURE_SIZE];
	char sig_b64[MANIFEST_B64_SIG_MAX];
	char pub_b64[MANIFEST_B64_PUB_MAX];
	const uint8_t *priv;
	const uint8_t *pub;
	char *out;
	int b64_len;

	priv = manifest_keys_private();
	pub = manifest_keys_public();
	if (!priv || !pub)
		return NULL;
	manifest = manifest_build_tree(cfg);
	if (!manifest)
		return NULL;
	if (jcs_canonicalize(manifest, &canonical, &canon_len) != 0) {
		cJSON_Delete(manifest);
		return NULL;
	}
	if (crypto_ed25519_sign(priv, CRYPTO_ED25519_PRIVATE_KEY_SIZE,
			canonical, canon_len, sig_raw, sizeof(sig_raw)) != 0) {
		free(canonical);
		cJSON_Delete(manifest);
		return NULL;
	}
	free(canonical);
	b64_len = crypto_base64_encode(sig_raw, sizeof(sig_raw), sig_b64, sizeof(sig_b64));
	if (b64_len < 0) {
		cJSON_Delete(manifest);
		return NULL;
	}
	b64_len = crypto_base64_encode(pub, CRYPTO_ED25519_PUBLIC_KEY_SIZE,
			pub_b64, sizeof(pub_b64));
	if (b64_len < 0) {
		cJSON_Delete(manifest);
		return NULL;
	}
	wrapper = cJSON_CreateObject();
	if (!wrapper) {
		cJSON_Delete(manifest);
		return NULL;
	}
	cJSON_AddItemToObject(wrapper, "manifest", manifest);
	sig_block = cJSON_CreateObject();
	if (!sig_block) {
		cJSON_Delete(wrapper);
		return NULL;
	}
	cJSON_AddItemToObject(wrapper, "signature", sig_block);
	{
		cJSON *alg_item = cJSON_CreateString(MANIFEST_SIG_ALG);
		cJSON *sig_item = cJSON_CreateString(sig_b64);
		cJSON *trust_item = cJSON_CreateString(MANIFEST_TRUST_SELF_SIGNED);
		cJSON *pub_item = cJSON_CreateString(pub_b64);

		if (!alg_item || !sig_item || !trust_item || !pub_item) {
			if (alg_item) cJSON_Delete(alg_item);
			if (sig_item) cJSON_Delete(sig_item);
			if (trust_item) cJSON_Delete(trust_item);
			if (pub_item) cJSON_Delete(pub_item);
			cJSON_Delete(wrapper);
			return NULL;
		}
		cJSON_AddItemToObject(sig_block, "alg", alg_item);
		cJSON_AddItemToObject(sig_block, "signature", sig_item);
		cJSON_AddItemToObject(sig_block, "trust_level", trust_item);
		cJSON_AddItemToObject(wrapper, "public_key", pub_item);
	}
	out = cJSON_PrintUnformatted(wrapper);
	cJSON_Delete(wrapper);
	return out;
}
