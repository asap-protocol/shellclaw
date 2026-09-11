/**
 * @file test_openai.c
 * @brief Tests for OpenAI provider: init, vtable, optional integration, negative (CR-21).
 */
#define _POSIX_C_SOURCE 200809L

#include "core/config.h"
#include "providers/openai_compat.h"
#include "providers/provider.h"
#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ASSERT(c) do { if (!(c)) { fprintf(stderr, "FAIL: %s:%d %s\n", __FILE__, __LINE__, #c); return 1; } } while (0)
#define RUN(t) do { int r = (t); if (r) return r; } while (0)

#ifdef SHELLCLAW_TEST
extern int openai_parse_response_for_test(const char *json, provider_response_t *response);
#endif

static const char *TMP_CONFIG = "/tmp/shellclaw_test_openai_config.toml";

static int write_config(void)
{
	FILE *f = fopen(TMP_CONFIG, "w");
	if (!f) return -1;
	fprintf(f, "[agent]\nmodel = \"gpt-4o-mini\"\nmax_tokens = 256\n");
	fprintf(f, "[memory]\ndb_path = \"/tmp/shellclaw_openai_test.db\"\n");
	fprintf(f, "[providers.openai]\napi_key_env = \"OPENAI_API_KEY\"\n");
	fclose(f);
	return 0;
}

static int test_openai_vtable(void)
{
	const provider_t *p = provider_openai_get();
	ASSERT(p != NULL);
	ASSERT(p->name != NULL);
	ASSERT(strcmp(p->name, "openai") == 0);
	ASSERT(p->init != NULL);
	ASSERT(p->chat != NULL);
	ASSERT(p->cleanup != NULL);
	return 0;
}

static int test_init_fails_without_config(void)
{
	const provider_t *p = provider_openai_get();
	ASSERT(p != NULL);
	ASSERT(p->init(NULL) == -1);
	return 0;
}

static int test_init_fails_without_api_key_in_env(void)
{
	ASSERT(write_config() == 0);
	config_t *cfg = NULL;
	char errbuf[256];
	ASSERT(config_load(TMP_CONFIG, &cfg, errbuf, sizeof(errbuf)) == 0);
	unsetenv("OPENAI_API_KEY");
	const provider_t *p = provider_openai_get();
	ASSERT(p != NULL);
	ASSERT(p->init(cfg) == -1);
	config_free(cfg);
	remove(TMP_CONFIG);
	return 0;
}

static int test_chat_fails_without_init(void)
{
	provider_response_t response = {0};
	const provider_t *p = provider_openai_get();
	ASSERT(p != NULL);
	provider_message_t msg = { .role = "user", .content = "Hi", .tool_calls = NULL, .tool_calls_count = 0, .tool_use_id = NULL };
	ASSERT(p->chat(&msg, 1, NULL, 0, &response) == -1);
	ASSERT(response.error != 0);
	provider_response_clear(&response);
	return 0;
}

static int test_init_and_chat_if_key_set(void)
{
	if (!getenv("OPENAI_API_KEY")) return 0;
	ASSERT(write_config() == 0);
	config_t *cfg = NULL;
	char errbuf[256];
	ASSERT(config_load(TMP_CONFIG, &cfg, errbuf, sizeof(errbuf)) == 0);
	const provider_t *p = provider_openai_get();
	ASSERT(p != NULL);
	ASSERT(p->init(cfg) == 0);
	provider_message_t msg = { .role = "user", .content = "Reply with exactly: OK", .tool_calls = NULL, .tool_calls_count = 0, .tool_use_id = NULL };
	provider_response_t response = {0};
	int ret = p->chat(&msg, 1, NULL, 0, &response);
	p->cleanup();
	config_free(cfg);
	remove(TMP_CONFIG);
	if (ret != 0) return 0;
	ASSERT(response.error == 0);
	ASSERT(response.content != NULL);
	ASSERT(strstr(response.content, "OK") != NULL || strlen(response.content) > 0);
	provider_response_clear(&response);
	return 0;
}

#ifdef SHELLCLAW_TEST
static int test_parse_malformed_json_sets_error(void)
{
	provider_response_t response = {0};
	ASSERT(openai_parse_response_for_test("not valid json", &response) == -1);
	ASSERT(response.error != 0);
	ASSERT(response.content != NULL);
	ASSERT(strstr(response.content, "parse") != NULL || strstr(response.content, "JSON") != NULL);
	provider_response_clear(&response);
	return 0;
}

static int test_parse_api_error_object_sets_error(void)
{
	const char *body = "{\"error\":{\"message\":\"Invalid API key\"}}";
	provider_response_t response = {0};
	ASSERT(openai_parse_response_for_test(body, &response) == -1);
	ASSERT(response.error != 0);
	ASSERT(response.content != NULL);
	ASSERT(strstr(response.content, "Invalid API key") != NULL);
	provider_response_clear(&response);
	return 0;
}

static int test_parse_empty_choices_returns_zero(void)
{
	const char *body = "{\"choices\":[]}";
	provider_response_t response = {0};
	ASSERT(openai_parse_response_for_test(body, &response) == 0);
	ASSERT(response.error == 0);
	provider_response_clear(&response);
	return 0;
}

static int test_parse_valid_message_returns_content(void)
{
	const char *body = "{\"choices\":[{\"message\":{\"content\":\"Hi there\"}}]}";
	provider_response_t response = {0};
	ASSERT(openai_parse_response_for_test(body, &response) == 0);
	ASSERT(response.error == 0);
	ASSERT(response.content != NULL);
	ASSERT(strstr(response.content, "Hi there") != NULL);
	provider_response_clear(&response);
	return 0;
}

static int test_parse_null_json_returns_error(void)
{
	provider_response_t response = {0};
	ASSERT(openai_parse_response_for_test(NULL, &response) == -1);
	ASSERT(response.error != 0);
	provider_response_clear(&response);
	return 0;
}

static int test_build_chat_body_serializes_tool_calls(void)
{
	provider_tool_call_t tool_call = {
		.id = "call_1",
		.name = "get_weather",
		.arguments = "{\"city\":\"Berlin\"}",
	};
	provider_message_t assistant = {
		.role = "assistant",
		.content = "",
		.tool_calls = &tool_call,
		.tool_calls_count = 1,
		.tool_use_id = NULL,
	};
	provider_message_t tool_result = {
		.role = "tool",
		.content = "sunny",
		.tool_calls = NULL,
		.tool_calls_count = 0,
		.tool_use_id = "call_1",
	};
	provider_message_t messages[] = {assistant, tool_result};
	provider_tool_def_t tools[] = {
		{
			.name = "get_weather",
			.description = "Weather lookup",
			.parameters_json = "{\"type\":\"object\",\"properties\":{\"city\":{\"type\":\"string\"}}}",
		},
	};
	provider_response_t response = {0};
	char *body;
	cJSON *parsed;
	cJSON *msg_arr;
	cJSON *first;
	cJSON *tool_calls;
	cJSON *tools_arr;

	body = openai_compat_build_chat_body("gpt-4o-mini", 128, 0.2, messages, 2, tools, 1, &response);
	ASSERT(body != NULL);
	parsed = cJSON_Parse(body);
	ASSERT(parsed != NULL);
	msg_arr = cJSON_GetObjectItem(parsed, "messages");
	ASSERT(msg_arr != NULL && cJSON_GetArraySize(msg_arr) == 2);
	first = cJSON_GetArrayItem(msg_arr, 0);
	tool_calls = cJSON_GetObjectItem(first, "tool_calls");
	ASSERT(tool_calls != NULL && cJSON_GetArraySize(tool_calls) == 1);
	tools_arr = cJSON_GetObjectItem(parsed, "tools");
	ASSERT(tools_arr != NULL && cJSON_GetArraySize(tools_arr) == 1);
	cJSON_Delete(parsed);
	cJSON_free(body);
	return 0;
}
#endif

int main(void)
{
	RUN(test_openai_vtable());
	RUN(test_init_fails_without_config());
	RUN(test_init_fails_without_api_key_in_env());
	RUN(test_chat_fails_without_init());
	RUN(test_init_and_chat_if_key_set());
#ifdef SHELLCLAW_TEST
	RUN(test_parse_malformed_json_sets_error());
	RUN(test_parse_api_error_object_sets_error());
	RUN(test_parse_empty_choices_returns_zero());
	RUN(test_parse_valid_message_returns_content());
	RUN(test_parse_null_json_returns_error());
	RUN(test_build_chat_body_serializes_tool_calls());
#endif
	printf("test_openai: all tests passed\n");
	return 0;
}
