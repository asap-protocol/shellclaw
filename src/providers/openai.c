/**
 * @file openai.c
 * @brief OpenAI provider: Chat Completions API, function/tool_calls.
 *
 * API key and endpoint from config; key from environment only. Never logged.
 */
#define _POSIX_C_SOURCE 200809L

#include "core/config.h"
#include "providers/openai_compat.h"
#include "providers/provider.h"
#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REQUEST_TIMEOUT_SEC 120
#define CONNECT_TIMEOUT_SEC 30
#define OPENAI_DEFAULT_ENDPOINT "https://api.openai.com/v1/chat/completions"

static char *s_oai_api_key;
static const config_t *s_oai_cfg;

#ifdef SHELLCLAW_TEST
static int s_oai_test_skip_http;
static char s_oai_test_model[160];

void openai_test_set_skip_http(int skip)
{
	s_oai_test_skip_http = skip ? 1 : 0;
}

const char *openai_test_last_request_model(void)
{
	return s_oai_test_model;
}

static void oai_test_store_model(const char *model)
{
	size_t n;

	s_oai_test_model[0] = '\0';
	if (!model)
		return;
	n = strlen(model);
	if (n >= sizeof(s_oai_test_model))
		n = sizeof(s_oai_test_model) - 1U;
	memcpy(s_oai_test_model, model, n);
	s_oai_test_model[n] = '\0';
}
#endif

static int build_and_send(const provider_message_t *messages, size_t message_count,
                          const provider_tool_def_t *tools, size_t tool_count,
                          provider_response_t *response)
{
	const char *endpoint;
	const char *configured;
	const char *model;
	int max_tokens;
	double temperature_cfg;
	char *body;
	int ret;
	if (!s_oai_cfg) {
		provider_set_error(response, "OpenAI provider not initialized");
		return -1;
	}
	endpoint = config_provider_openai_endpoint(s_oai_cfg);
	if (!endpoint || !endpoint[0])
		endpoint = OPENAI_DEFAULT_ENDPOINT;
	configured = config_agent_model(s_oai_cfg);
	if (!configured || configured[0] == '\0')
		configured = "gpt-4o-mini";
	/* Stripping "openai/..." or skipping another vendor's prefix is a fix for
	 * the real OpenAI API, which has no slash ids. An OpenAI-compatible
	 * gateway (e.g. OpenRouter via a custom endpoint) routes on the vendor
	 * prefix, so it needs the configured id verbatim. */
	if (strcmp(endpoint, OPENAI_DEFAULT_ENDPOINT) == 0)
		model = provider_model_id_for_backend(configured, "openai");
	else
		model = configured;
	if (!model) {
		char errmsg[256];
		snprintf(errmsg, sizeof(errmsg),
		         "OpenAI skipped: agent model \"%s\" is not an OpenAI id",
		         configured);
		provider_set_error(response, errmsg);
		return -1;
	}
#ifdef SHELLCLAW_TEST
	if (s_oai_test_skip_http) {
		oai_test_store_model(model);
		response->content = provider_dup_str("");
		if (!response->content) {
			provider_set_error(response, "Out of memory");
			return -1;
		}
		return 0;
	}
#endif
	max_tokens = config_agent_max_tokens(s_oai_cfg);
	temperature_cfg = config_agent_temperature(s_oai_cfg);
	body = openai_compat_build_chat_body(model, max_tokens, temperature_cfg, messages,
	                                     message_count, tools, tool_count, response);
	if (!body)
		return -1;
	ret = openai_compat_post_chat(endpoint, body, s_oai_api_key, "OpenAI API", 0,
	                              REQUEST_TIMEOUT_SEC, CONNECT_TIMEOUT_SEC, response);
	cJSON_free(body);
	return ret;
}

static int openai_init(const config_t *cfg)
{
	const char *env_name;
	const char *key;
	if (!cfg)
		return -1;
	env_name = config_provider_openai_api_key_env(cfg);
	if (!env_name || !env_name[0])
		return -1;
	key = getenv(env_name);
	if (!key || !key[0])
		return -1;
	free(s_oai_api_key);
	s_oai_api_key = strdup(key);
	s_oai_cfg = s_oai_api_key ? cfg : NULL;
	return s_oai_api_key ? 0 : -1;
}

static int openai_chat(const provider_message_t *messages, size_t message_count,
                       const provider_tool_def_t *tools, size_t tool_count,
                       provider_response_t *response)
{
	if (!s_oai_api_key || !s_oai_cfg) {
		provider_set_error(response, "OpenAI provider not initialized or API key missing");
		return -1;
	}
	provider_response_clear(response);
	return build_and_send(messages, message_count, tools, tool_count, response);
}

static void openai_cleanup(void)
{
	if (s_oai_api_key) {
		volatile char *p = (volatile char *)s_oai_api_key;
		for (size_t i = 0; s_oai_api_key[i] != '\0'; i++)
			p[i] = '\0';
		free(s_oai_api_key);
		s_oai_api_key = NULL;
	}
	s_oai_cfg = NULL;
}

static const provider_t openai_provider = {
	.name = "openai",
	.init = openai_init,
	.chat = openai_chat,
	.cleanup = openai_cleanup,
};

const provider_t *provider_openai_get(void)
{
	return &openai_provider;
}

void provider_openai_set_live_config(const config_t *cfg)
{
	if (cfg && s_oai_api_key)
		s_oai_cfg = cfg;
}

#ifdef SHELLCLAW_TEST
int openai_parse_response_for_test(const char *json, provider_response_t *response)
{
	return provider_parse_chat_completions_json(json, response);
}
#endif
