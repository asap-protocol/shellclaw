/**
 * @file test_local_provider.c
 * @brief Local (llama-server) provider: unreachable init does not abort; chat rejects when marked down.
 */

#define _POSIX_C_SOURCE 200809L

#include "core/config.h"
#include "providers/provider.h"
#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ASSERT(c) do { if (!(c)) { fprintf(stderr, "FAIL: %s:%d %s\n", __FILE__, __LINE__, #c); return 1; } } while (0)
#define RUN(t) do { int r = (t); if (r) return r; } while (0)

#ifdef SHELLCLAW_TEST
extern int local_provider_is_unavailable_for_test(void);
#endif

static const char *TMP_CFG = "/tmp/shellclaw_test_local_provider.toml";

static int test_local_provider_vtable(void)
{
	const provider_t *p = provider_local_get();
	ASSERT(p != NULL);
	ASSERT(p->name != NULL);
	ASSERT(strcmp(p->name, "local") == 0);
	ASSERT(p->init != NULL);
	ASSERT(p->chat != NULL);
	ASSERT(p->cleanup != NULL);
	return 0;
}

static int test_init_requires_config(void)
{
	const provider_t *p = provider_local_get();
	ASSERT(p != NULL);
	ASSERT(p->init(NULL) == -1);
	return 0;
}

/** Port 1 is almost always closed locally; probe should fail without blocking startup. */
static int test_init_unreachable_sets_unavailable_then_chat_errors(void)
{
	FILE *f = fopen(TMP_CFG, "w");
	ASSERT(f);
	unsetenv("SHELLCLAW_FALLBACK_CHAIN");
	unsetenv("SHELLCLAW_LOCAL_ENDPOINT");
	unsetenv("SHELLCLAW_LOCAL_MODEL");
	fprintf(f, "[agent]\nmodel = \"test-model\"\n");
	fprintf(f, "[providers.local]\n");
	fprintf(f, "endpoint = \"http://127.0.0.1:1/v1/chat/completions\"\nmodel = \"tiny\"\n");
	fclose(f);
	config_t *cfg = NULL;
	char errbuf[256];
	ASSERT(config_load(TMP_CFG, &cfg, errbuf, sizeof(errbuf)) == 0);
	ASSERT(cfg != NULL);
	const provider_t *p = provider_local_get();
	ASSERT(p->init(cfg) == 0);
#ifdef SHELLCLAW_TEST
	ASSERT(local_provider_is_unavailable_for_test() != 0);
#endif
	provider_message_t msg = { .role = "user", .content = "hello", .tool_calls = NULL, .tool_calls_count = 0, .tool_use_id = NULL };
	provider_response_t response = {0};
	ASSERT(p->chat(&msg, 1, NULL, 0, &response) == -1);
	ASSERT(response.error != 0);
	ASSERT(response.content != NULL);
	ASSERT(strstr(response.content, "unreachable") != NULL);
	p->cleanup();
	provider_response_clear(&response);
	config_free(cfg);
	remove(TMP_CFG);
	return 0;
}

/**
 * Init probe failure must not stick for the process lifetime. llama-server
 * often is not listening yet at startup; the next chat has to probe again.
 */
static int test_chat_reprobes_when_server_comes_up(void)
{
	FILE *f = fopen(TMP_CFG, "w");
	config_t *cfg = NULL;
	char errbuf[256];
	const provider_t *p;
	provider_message_t msg = { .role = "user", .content = "ping", .tool_calls = NULL,
	                           .tool_calls_count = 0, .tool_use_id = NULL };
	provider_response_t response = {0};
	ASSERT(f);
	local_provider_test_reset();
	unsetenv("SHELLCLAW_FALLBACK_CHAIN");
	unsetenv("SHELLCLAW_LOCAL_ENDPOINT");
	unsetenv("SHELLCLAW_LOCAL_MODEL");
	fprintf(f, "[agent]\nmodel = \"test-model\"\n");
	fprintf(f, "[providers.local]\n");
	fprintf(f, "endpoint = \"http://127.0.0.1:1/v1/chat/completions\"\nmodel = \"tiny\"\n");
	fclose(f);
	ASSERT(config_load(TMP_CFG, &cfg, errbuf, sizeof(errbuf)) == 0);
	p = provider_local_get();
	ASSERT(p->init(cfg) == 0);
#ifdef SHELLCLAW_TEST
	ASSERT(local_provider_is_unavailable_for_test() != 0);
#endif
	local_provider_test_set_skip_probe(1);
	local_provider_test_set_http_response(
		"{\"choices\":[{\"message\":{\"role\":\"assistant\",\"content\":\"back online\"}}]}");
	ASSERT(p->chat(&msg, 1, NULL, 0, &response) == 0);
	ASSERT(response.error == 0);
	ASSERT(response.content != NULL);
	ASSERT(strstr(response.content, "back online") != NULL);
#ifdef SHELLCLAW_TEST
	ASSERT(local_provider_is_unavailable_for_test() == 0);
#endif
	p->cleanup();
	provider_response_clear(&response);
	local_provider_test_reset();
	config_free(cfg);
	remove(TMP_CFG);
	return 0;
}

/**
 * While unavailable, chat re-probes at most once per LOCAL_REPROBE_MIN_SEC:
 * agent_run holds agent_lock during chat, so a down llama-server must not
 * cost synchronous curl probes on every turn. The second chat inside the
 * window must not probe (skip_probe would otherwise recover it through the
 * mock); after the test hook resets the throttle it must probe again.
 */
static int test_chat_reprobe_throttled_within_min_interval(void)
{
	FILE *f = fopen(TMP_CFG, "w");
	config_t *cfg = NULL;
	char errbuf[256];
	const provider_t *p;
	provider_message_t msg = { .role = "user", .content = "ping", .tool_calls = NULL,
	                           .tool_calls_count = 0, .tool_use_id = NULL };
	provider_response_t response = {0};
	ASSERT(f);
	local_provider_test_reset();
	unsetenv("SHELLCLAW_FALLBACK_CHAIN");
	unsetenv("SHELLCLAW_LOCAL_ENDPOINT");
	unsetenv("SHELLCLAW_LOCAL_MODEL");
	fprintf(f, "[agent]\nmodel = \"test-model\"\n");
	fprintf(f, "[providers.local]\n");
	fprintf(f, "endpoint = \"http://127.0.0.1:1/v1/chat/completions\"\nmodel = \"tiny\"\n");
	fclose(f);
	ASSERT(config_load(TMP_CFG, &cfg, errbuf, sizeof(errbuf)) == 0);
	p = provider_local_get();
	ASSERT(p->init(cfg) == 0);
#ifdef SHELLCLAW_TEST
	ASSERT(local_provider_is_unavailable_for_test() != 0);
#endif
	/* First chat after the startup miss: the probe runs against port 1
	 * and fails, arming the throttle window. */
	ASSERT(p->chat(&msg, 1, NULL, 0, &response) == -1);
	ASSERT(response.error != 0);
	ASSERT(strstr(response.content, "unreachable") != NULL);
	provider_response_clear(&response);
	/* Second chat inside the window: no probe, so skip_probe cannot clear
	 * the flag and the mock is not served. */
	local_provider_test_set_skip_probe(1);
	local_provider_test_set_http_response(
		"{\"choices\":[{\"message\":{\"role\":\"assistant\",\"content\":\"back online\"}}]}");
	ASSERT(p->chat(&msg, 1, NULL, 0, &response) == -1);
	ASSERT(response.error != 0);
	ASSERT(strstr(response.content, "unreachable") != NULL);
	provider_response_clear(&response);
	/* After the hook resets the throttle and init re-marks unavailable,
	 * the next chat must probe again and recover through the mock. */
	local_provider_test_reset();
	ASSERT(p->init(cfg) == 0);
#ifdef SHELLCLAW_TEST
	ASSERT(local_provider_is_unavailable_for_test() != 0);
#endif
	local_provider_test_set_skip_probe(1);
	local_provider_test_set_http_response(
		"{\"choices\":[{\"message\":{\"role\":\"assistant\",\"content\":\"back online\"}}]}");
	ASSERT(p->chat(&msg, 1, NULL, 0, &response) == 0);
	ASSERT(response.error == 0);
	ASSERT(response.content != NULL);
	ASSERT(strstr(response.content, "back online") != NULL);
	p->cleanup();
	provider_response_clear(&response);
	local_provider_test_reset();
	config_free(cfg);
	remove(TMP_CFG);
	return 0;
}

static int test_chat_mock_http_happy_path(void)
{
	FILE *f = fopen(TMP_CFG, "w");
	ASSERT(f);
	local_provider_test_reset();
	fprintf(f, "[agent]\nmodel = \"test-model\"\n");
	fprintf(f, "[providers.local]\n");
	fprintf(f, "endpoint = \"http://127.0.0.1:8080/v1/chat/completions\"\nmodel = \"tiny\"\n");
	fclose(f);
	config_t *cfg = NULL;
	char errbuf[256];
	ASSERT(config_load(TMP_CFG, &cfg, errbuf, sizeof(errbuf)) == 0);
	const provider_t *p = provider_local_get();
	local_provider_test_set_skip_probe(1);
	local_provider_test_set_http_response(
		"{\"choices\":[{\"message\":{\"role\":\"assistant\",\"content\":\"hello from mock\"}}]}");
	ASSERT(p->init(cfg) == 0);
	provider_message_t msg = { .role = "user", .content = "ping", .tool_calls = NULL,
	                           .tool_calls_count = 0, .tool_use_id = NULL };
	provider_response_t response = {0};
	ASSERT(p->chat(&msg, 1, NULL, 0, &response) == 0);
	ASSERT(response.error == 0);
	ASSERT(response.content != NULL);
	ASSERT(strstr(response.content, "hello from mock") != NULL);
	p->cleanup();
	provider_response_clear(&response);
	local_provider_test_reset();
	config_free(cfg);
	remove(TMP_CFG);
	return 0;
}

int main(void)
{
	ASSERT(curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK);
	RUN(test_local_provider_vtable());
	RUN(test_init_requires_config());
	RUN(test_init_unreachable_sets_unavailable_then_chat_errors());
	RUN(test_chat_reprobes_when_server_comes_up());
	RUN(test_chat_reprobe_throttled_within_min_interval());
	RUN(test_chat_mock_http_happy_path());
	curl_global_cleanup();
	printf("test_local_provider: all tests passed\n");
	return 0;
}
