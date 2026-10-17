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

static char *s_oai_api_key;
static const config_t *s_oai_cfg;

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
	char errmsg[256];
	int ret;
	if (!s_oai_cfg) {
		provider_set_error(response, "OpenAI provider not initialized");
		return -1;
	}
	endpoint = config_provider_openai_endpoint(s_oai_cfg);
	if (!endpoint || !endpoint[0])
		endpoint = "https://api.openai.com/v1/chat/completions";
	configured = config_agent_model(s_oai_cfg);
	if (!configured || configured[0] == '\0')
		configured = "gpt-4o-mini";
	model = provider_model_id_for_backend(configured, "openai");
	if (!model) {
		snprintf(errmsg, sizeof(errmsg),
		         "OpenAI skipped: agent model \"%s\" is not an OpenAI id",
		         configured);
		provider_set_error(response, errmsg);
		return -1;
	}
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
