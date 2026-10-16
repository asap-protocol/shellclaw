/**
 * @file local.c
 * @brief OpenAI-compatible Chat Completions client for llama-server; init probe marks unreachable without failing startup.
 * A later chat probes again so a server that was down only at startup can still serve.
 */

#define _POSIX_C_SOURCE 200809L

#include "core/config.h"
#include "providers/openai_compat.h"
#include "providers/provider.h"
#include "cJSON.h"
#include <curl/curl.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define REQUEST_TIMEOUT_SEC 120
#define CONNECT_TIMEOUT_SEC 30
#define LOCAL_PROBE_TIMEOUT_SEC 3L
#define LOCAL_PROBE_CONNECT_SEC 2L

static const config_t *s_local_cfg;
static int s_local_unavailable;
#ifdef SHELLCLAW_TEST
static const char *s_local_test_http_body;
static int s_local_test_skip_probe;
#endif

static size_t local_discard_write_cb(const char *ptr, size_t size, size_t nmemb, void *userdata)
{
	(void)ptr;
	(void)userdata;
	if (nmemb != 0 && size > SIZE_MAX / nmemb)
		return 0;
	return size * nmemb;
}

static int local_probe_url(const char *url)
{
	CURL *curl;
	CURLcode res;
	if (!url || !url[0])
		return 0;
	curl = curl_easy_init();
	if (!curl)
		return 0;
	curl_easy_setopt(curl, CURLOPT_URL, url);
	curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
	curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
	curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
	curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, LOCAL_PROBE_TIMEOUT_SEC);
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, LOCAL_PROBE_CONNECT_SEC);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, local_discard_write_cb);
	res = curl_easy_perform(curl);
	curl_easy_cleanup(curl);
	return res == CURLE_OK ? 1 : 0;
}

static int local_build_models_url(const char *endpoint, char *buf, size_t bufsz)
{
	const char *v1 = endpoint ? strstr(endpoint, "/v1/") : NULL;
	size_t prefix;
	if (!endpoint || !endpoint[0] || !v1)
		return -1;
	prefix = (size_t)(v1 - endpoint) + 3U;
	if (prefix + 8U >= bufsz)
		return -1;
	memcpy(buf, endpoint, prefix);
	buf[prefix] = '\0';
	strncat(buf, "/models", bufsz - prefix - 1U);
	return 0;
}

static int local_build_health_url(const char *endpoint, char *buf, size_t bufsz)
{
	const char *v1 = endpoint ? strstr(endpoint, "/v1/") : NULL;
	size_t prefix;
	if (!endpoint || !endpoint[0])
		return -1;
	if (v1) {
		prefix = (size_t)(v1 - endpoint);
	} else {
		const char *path = strstr(endpoint, "://");
		if (!path)
			return -1;
		path += 3;
		while (*path && *path != '/')
			path++;
		prefix = (size_t)(path - endpoint);
	}
	if (prefix + 8U >= bufsz)
		return -1;
	memcpy(buf, endpoint, prefix);
	buf[prefix] = '\0';
	strncat(buf, "/health", bufsz - prefix - 1U);
	return 0;
}

static int local_probe_endpoint(const char *endpoint)
{
#ifdef SHELLCLAW_TEST
	if (s_local_test_skip_probe)
		return 1;
#endif
	char models_url[512];
	char health_url[512];
	if (!endpoint || !endpoint[0])
		return 0;
	if (local_build_models_url(endpoint, models_url, sizeof(models_url)) == 0 &&
	    local_probe_url(models_url))
		return 1;
	if (local_build_health_url(endpoint, health_url, sizeof(health_url)) == 0 &&
	    local_probe_url(health_url))
		return 1;
	return 0;
}

int provider_local_endpoint_reachable(const char *endpoint)
{
	return local_probe_endpoint(endpoint);
}

static int local_build_and_send(const provider_message_t *messages, size_t message_count,
                                const provider_tool_def_t *tools, size_t tool_count,
                                provider_response_t *response)
{
	const char *endpoint;
	const char *model;
	int max_tokens;
	double temperature_cfg;
	char *body;
	int ret;
	if (!s_local_cfg) {
		provider_set_error(response, "Local provider not initialized");
		return -1;
	}
	endpoint = config_provider_local_endpoint(s_local_cfg);
	model = config_provider_local_model(s_local_cfg);
	if (!endpoint || endpoint[0] == '\0' || !model || model[0] == '\0') {
		provider_set_error(response, "Local provider endpoint or model not configured");
		return -1;
	}
	max_tokens = config_agent_max_tokens(s_local_cfg);
	temperature_cfg = config_agent_temperature(s_local_cfg);
	body = openai_compat_build_chat_body(model, max_tokens, temperature_cfg, messages,
	                                     message_count, tools, tool_count, response);
	if (!body)
		return -1;
#ifdef SHELLCLAW_TEST
	if (s_local_test_http_body != NULL) {
		ret = provider_parse_chat_completions_json(s_local_test_http_body, response);
		cJSON_free(body);
		return ret;
	}
#endif
	ret = openai_compat_post_chat(endpoint, body, NULL, "Local provider", 1, REQUEST_TIMEOUT_SEC,
	                              CONNECT_TIMEOUT_SEC, response);
	cJSON_free(body);
	return ret;
}

static int local_init(const config_t *cfg)
{
	const char *endpoint;
	if (!cfg)
		return -1;
	s_local_cfg = cfg;
	endpoint = config_provider_local_endpoint(cfg);
	if (!endpoint || endpoint[0] == '\0') {
		s_local_unavailable = 1;
		return 0;
	}
	s_local_unavailable = local_probe_endpoint(endpoint) ? 0 : 1;
	return 0;
}

static int local_chat(const provider_message_t *messages, size_t message_count,
                      const provider_tool_def_t *tools, size_t tool_count,
                      provider_response_t *response)
{
	if (!s_local_cfg) {
		provider_set_error(response, "Local provider not initialized");
		return -1;
	}
	/* Init probes once, often before llama-server has loaded the model.
	 * Default fallback order puts local last, so router recovery (which
	 * only probes backends before the active index) never clears the flag. */
	if (s_local_unavailable) {
		const char *endpoint = config_provider_local_endpoint(s_local_cfg);
		if (endpoint && endpoint[0] != '\0' && provider_local_endpoint_reachable(endpoint))
			provider_local_recovery_probe_succeeded();
	}
	if (s_local_unavailable) {
		provider_set_error(response, "Local inference server unreachable");
		return -1;
	}
	provider_response_clear(response);
	return local_build_and_send(messages, message_count, tools, tool_count, response);
}

static void local_cleanup(void)
{
	s_local_cfg = NULL;
	s_local_unavailable = 0;
}

#ifdef SHELLCLAW_TEST
int local_provider_is_unavailable_for_test(void)
{
	return s_local_unavailable;
}

void local_provider_test_reset(void)
{
	s_local_test_http_body = NULL;
	s_local_test_skip_probe = 0;
	s_local_unavailable = 0;
}

void local_provider_test_set_http_response(const char *json_body)
{
	s_local_test_http_body = json_body;
}

void local_provider_test_set_skip_probe(int skip)
{
	s_local_test_skip_probe = skip ? 1 : 0;
}
#endif

static const provider_t local_provider = {
	.name = "local",
	.init = local_init,
	.chat = local_chat,
	.cleanup = local_cleanup,
};

const provider_t *provider_local_get(void)
{
	return &local_provider;
}

void provider_local_set_live_config(const config_t *cfg)
{
	if (cfg)
		s_local_cfg = cfg;
}

void provider_local_recovery_probe_succeeded(void)
{
	s_local_unavailable = 0;
}
