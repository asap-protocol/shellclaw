/**
 * @file manifest_test_common.h
 * @brief Shared helpers for manifest / JCS unit tests.
 */
#ifndef SHELLCLAW_TESTS_MANIFEST_TEST_COMMON_H
#define SHELLCLAW_TESTS_MANIFEST_TEST_COMMON_H

#include "asap/manifest.h"
#include "cJSON.h"
#include <stdio.h>
#include <string.h>

#define ASSERT(c) do { \
	if (!(c)) { \
		fprintf(stderr, "FAIL: %s:%d %s\n", __FILE__, __LINE__, #c); \
		return 1; \
	} \
} while (0)

#define TMP_CONFIG "/tmp/shellclaw_test_manifest_config.toml"
#define MANIFEST_ASAP_VERSION "2.1.0"

static int assert_manifest_shape(cJSON *parsed, const char *expect_id,
	const char *expect_version, const char *expect_hw_class,
	const char *expect_hw_model, const char *expect_mode0,
	const char *expect_local_model)
{
	cJSON *id;
	cJSON *version;
	cJSON *description;
	cJSON *capabilities;
	cJSON *skills;
	cJSON *hardware;
	cJSON *inference;
	cJSON *modes;
	cJSON *local_models;
	cJSON *endpoints;
	cJSON *asap_ep;
	cJSON *state_persistence;
	cJSON *streaming;
	cJSON *mcp_tools;
	cJSON *ttl_seconds;
	cJSON *supported_versions;

	id = cJSON_GetObjectItem(parsed, "id");
	ASSERT(id != NULL && cJSON_IsString(id));
	ASSERT(strcmp(id->valuestring, expect_id) == 0);
	version = cJSON_GetObjectItem(parsed, "version");
	ASSERT(version != NULL && cJSON_IsString(version));
	ASSERT(strcmp(version->valuestring, expect_version) == 0);
	description = cJSON_GetObjectItem(parsed, "description");
	ASSERT(description != NULL && cJSON_IsString(description));
	ASSERT(description->valuestring[0] != '\0');
	ASSERT(cJSON_GetObjectItem(parsed, "skills") == NULL);
	capabilities = cJSON_GetObjectItem(parsed, "capabilities");
	ASSERT(capabilities != NULL && cJSON_IsObject(capabilities));
	ASSERT(strcmp(cJSON_GetObjectItem(capabilities, "asap_version")->valuestring,
		MANIFEST_ASAP_VERSION) == 0);
	skills = cJSON_GetObjectItem(capabilities, "skills");
	ASSERT(skills != NULL && cJSON_IsArray(skills));
	if (cJSON_GetArraySize(skills) > 0) {
		cJSON *first = cJSON_GetArrayItem(skills, 0);
		ASSERT(cJSON_GetObjectItem(first, "id") != NULL);
		ASSERT(cJSON_GetObjectItem(first, "description") != NULL);
	}
	state_persistence = cJSON_GetObjectItem(capabilities, "state_persistence");
	ASSERT(state_persistence != NULL && cJSON_IsFalse(state_persistence));
	streaming = cJSON_GetObjectItem(capabilities, "streaming");
	ASSERT(streaming != NULL && cJSON_IsFalse(streaming));
	mcp_tools = cJSON_GetObjectItem(capabilities, "mcp_tools");
	ASSERT(mcp_tools != NULL && cJSON_IsArray(mcp_tools));
	ASSERT(cJSON_GetArraySize(mcp_tools) == 0);
	hardware = cJSON_GetObjectItem(capabilities, "hardware");
	ASSERT(hardware != NULL && cJSON_IsObject(hardware));
	ASSERT(strcmp(cJSON_GetObjectItem(hardware, "class_")->valuestring, expect_hw_class) == 0);
	ASSERT(strcmp(cJSON_GetObjectItem(hardware, "model")->valuestring, expect_hw_model) == 0);
	ASSERT(cJSON_GetObjectItem(hardware, "io") != NULL);
	inference = cJSON_GetObjectItem(capabilities, "inference");
	ASSERT(inference != NULL && cJSON_IsObject(inference));
	modes = cJSON_GetObjectItem(inference, "modes");
	ASSERT(modes != NULL && cJSON_IsArray(modes));
	ASSERT(cJSON_GetArraySize(modes) >= 1);
	ASSERT(strcmp(cJSON_GetArrayItem(modes, 0)->valuestring, expect_mode0) == 0);
	local_models = cJSON_GetObjectItem(inference, "local_models");
	ASSERT(local_models != NULL && cJSON_GetArraySize(local_models) == 1);
	ASSERT(strcmp(cJSON_GetObjectItem(cJSON_GetArrayItem(local_models, 0), "id")->valuestring,
		expect_local_model) == 0);
	endpoints = cJSON_GetObjectItem(parsed, "endpoints");
	ASSERT(endpoints != NULL && cJSON_IsObject(endpoints));
	asap_ep = cJSON_GetObjectItem(endpoints, "asap");
	ASSERT(asap_ep != NULL && cJSON_IsString(asap_ep));
	ASSERT(strstr(asap_ep->valuestring, "://") != NULL);
	ASSERT(strstr(asap_ep->valuestring, "/asap") != NULL);
	ASSERT(cJSON_GetObjectItem(endpoints, "health") == NULL);
	ASSERT(cJSON_GetObjectItem(endpoints, "manifest") == NULL);
	/* Upstream default-populated fields (SIG path A): ttl_seconds and
	 * supported_versions must be present with the upstream defaults. */
	ttl_seconds = cJSON_GetObjectItem(parsed, "ttl_seconds");
	ASSERT(ttl_seconds != NULL && cJSON_IsNumber(ttl_seconds));
	ASSERT(ttl_seconds->valuedouble == 300.0);
	supported_versions = cJSON_GetObjectItem(parsed, "supported_versions");
	ASSERT(supported_versions != NULL && cJSON_IsArray(supported_versions));
	ASSERT(cJSON_GetArraySize(supported_versions) >= 1);
	return 0;
}

static int is_valid_base64(const char *s)
{
	size_t len;
	size_t i;
	if (!s || s[0] == '\0')
		return 0;
	len = strlen(s);
	if (len % 4U != 0U)
		return 0;
	for (i = 0; i < len; i++) {
		char c = s[i];
		if (c == '=') {
			if (i < len - 2U)
				return 0;
			continue;
		}
		if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
		    (c >= '0' && c <= '9') || c == '+' || c == '/')
			continue;
		return 0;
	}
	return 1;
}

#endif /* SHELLCLAW_TESTS_MANIFEST_TEST_COMMON_H */
