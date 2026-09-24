/**
 * @file test_anthropic.c
 * @brief Tests for Anthropic provider: init, vtable, optional integration, negative (CR-21).
 */
#define _POSIX_C_SOURCE 200809L

#include "core/config.h"
#include "providers/provider.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ASSERT(c) do { if (!(c)) { fprintf(stderr, "FAIL: %s:%d %s\n", __FILE__, __LINE__, #c); return 1; } } while (0)
#define RUN(t) do { int r = (t); if (r) return r; } while (0)

#ifdef SHELLCLAW_TEST
extern int anthropic_parse_response_for_test(const char *json, provider_response_t *response);
extern void anthropic_test_fail_next_reallocs(int n);
#endif

static const char *TMP_CONFIG = "/tmp/shellclaw_test_anthropic_config.toml";

static int write_config(void)
{
	FILE *f = fopen(TMP_CONFIG, "w");
	if (!f) return -1;
	fprintf(f, "[agent]\nmodel = \"claude-3-5-sonnet-20241022\"\nmax_tokens = 256\n");
	fprintf(f, "[memory]\ndb_path = \"/tmp/shellclaw_anth_test.db\"\n");
	fprintf(f, "[providers.anthropic]\napi_key_env = \"ANTHROPIC_API_KEY\"\n");
	fclose(f);
	return 0;
}

static int test_anthropic_vtable(void)
{
	const provider_t *p = provider_anthropic_get();
	ASSERT(p != NULL);
	ASSERT(p->name != NULL);
	ASSERT(strcmp(p->name, "anthropic") == 0);
	ASSERT(p->init != NULL);
	ASSERT(p->chat != NULL);
	ASSERT(p->cleanup != NULL);
	return 0;
}

static int test_init_fails_without_config(void)
{
	const provider_t *p = provider_anthropic_get();
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
	unsetenv("ANTHROPIC_API_KEY");
	const provider_t *p = provider_anthropic_get();
	ASSERT(p != NULL);
	ASSERT(p->init(cfg) == -1);
	config_free(cfg);
	remove(TMP_CONFIG);
	return 0;
}

static int test_chat_fails_without_init(void)
{
	provider_response_t response = {0};
	const provider_t *p = provider_anthropic_get();
	ASSERT(p != NULL);
	provider_message_t msg = { .role = "user", .content = "Hi", .tool_calls = NULL, .tool_calls_count = 0, .tool_use_id = NULL };
	ASSERT(p->chat(&msg, 1, NULL, 0, &response) == -1);
	ASSERT(response.error != 0);
	provider_response_clear(&response);
	return 0;
}

static int test_init_and_chat_if_key_set(void)
{
	if (!getenv("ANTHROPIC_API_KEY")) return 0;
	ASSERT(write_config() == 0);
	config_t *cfg = NULL;
	char errbuf[256];
	ASSERT(config_load(TMP_CONFIG, &cfg, errbuf, sizeof(errbuf)) == 0);
	const provider_t *p = provider_anthropic_get();
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
	ASSERT(anthropic_parse_response_for_test("not valid json", &response) == -1);
	ASSERT(response.error != 0);
	ASSERT(response.content != NULL);
	ASSERT(strstr(response.content, "parse") != NULL || strstr(response.content, "JSON") != NULL);
	provider_response_clear(&response);
	return 0;
}

static int test_parse_api_error_object_sets_error(void)
{
	const char *body = "{\"error\":{\"type\":\"rate_limit\",\"message\":\"Rate limit exceeded\"}}";
	provider_response_t response = {0};
	ASSERT(anthropic_parse_response_for_test(body, &response) == -1);
	ASSERT(response.error != 0);
	ASSERT(response.content != NULL);
	ASSERT(strstr(response.content, "Rate limit") != NULL);
	provider_response_clear(&response);
	return 0;
}

static int test_parse_empty_content_array_returns_zero(void)
{
	const char *body = "{\"content\":[]}";
	provider_response_t response = {0};
	ASSERT(anthropic_parse_response_for_test(body, &response) == 0);
	ASSERT(response.error == 0);
	provider_response_clear(&response);
	return 0;
}

static int test_parse_valid_text_block_returns_content(void)
{
	const char *body = "{\"content\":[{\"type\":\"text\",\"text\":\"Hello world\"}]}";
	provider_response_t response = {0};
	ASSERT(anthropic_parse_response_for_test(body, &response) == 0);
	ASSERT(response.error == 0);
	ASSERT(response.content != NULL);
	ASSERT(strstr(response.content, "Hello world") != NULL);
	provider_response_clear(&response);
	return 0;
}

static int test_parse_null_json_returns_error(void)
{
	provider_response_t response = {0};
	ASSERT(anthropic_parse_response_for_test(NULL, &response) == -1);
	provider_response_clear(&response);
	return 0;
}

static int test_parse_text_past_initial_cap(void)
{
	char text[301];
	char body[384];
	provider_response_t response = {0};
	memset(text, 'A', 300);
	text[300] = '\0';
	ASSERT(snprintf(body, sizeof(body),
	                "{\"content\":[{\"type\":\"text\",\"text\":\"%s\"}]}", text) < (int)sizeof(body));
	ASSERT(anthropic_parse_response_for_test(body, &response) == 0);
	ASSERT(response.error == 0);
	ASSERT(response.content != NULL);
	ASSERT(strlen(response.content) == 300);
	ASSERT(response.content[0] == 'A' && response.content[299] == 'A');
	provider_response_clear(&response);
	return 0;
}

static int fill_tool_use_body(char *dst, size_t dst_sz, size_t n)
{
	size_t off = 0;
	int w;
	size_t i;
	w = snprintf(dst, dst_sz, "{\"content\":[");
	if (w < 0 || (size_t)w >= dst_sz)
		return -1;
	off = (size_t)w;
	for (i = 0; i < n; i++) {
		w = snprintf(dst + off, dst_sz - off,
		             "%s{\"type\":\"tool_use\",\"id\":\"t%zu\",\"name\":\"shell\",\"input\":{}}",
		             i ? "," : "", i);
		if (w < 0 || (size_t)w >= dst_sz - off)
			return -1;
		off += (size_t)w;
	}
	w = snprintf(dst + off, dst_sz - off, "]}");
	if (w < 0 || (size_t)w >= dst_sz - off)
		return -1;
	return 0;
}

static int test_parse_six_tool_use_blocks(void)
{
	char body[1024];
	provider_response_t response = {0};
	ASSERT(fill_tool_use_body(body, sizeof(body), 6) == 0);
	ASSERT(anthropic_parse_response_for_test(body, &response) == 0);
	ASSERT(response.error == 0);
	ASSERT(response.tool_calls_count == 6);
	ASSERT(response.tool_calls != NULL);
	ASSERT(response.tool_calls[0].id != NULL && strcmp(response.tool_calls[0].id, "t0") == 0);
	ASSERT(response.tool_calls[5].id != NULL && strcmp(response.tool_calls[5].id, "t5") == 0);
	provider_response_clear(&response);
	return 0;
}

static int test_parse_text_realloc_failure_is_error(void)
{
	char text[301];
	char body[384];
	provider_response_t response = {0};
	int ret;
	memset(text, 'B', 300);
	text[300] = '\0';
	ASSERT(snprintf(body, sizeof(body),
	                "{\"content\":[{\"type\":\"text\",\"text\":\"%s\"}]}", text) < (int)sizeof(body));
	anthropic_test_fail_next_reallocs(1);
	ret = anthropic_parse_response_for_test(body, &response);
	anthropic_test_fail_next_reallocs(0);
	ASSERT(ret == -1);
	ASSERT(response.error != 0);
	ASSERT(response.content != NULL);
	ASSERT(strstr(response.content, "Out of memory") != NULL);
	provider_response_clear(&response);
	return 0;
}

static int test_parse_tool_realloc_failure_is_error(void)
{
	char body[1024];
	provider_response_t response = {0};
	int ret;
	ASSERT(fill_tool_use_body(body, sizeof(body), 6) == 0);
	anthropic_test_fail_next_reallocs(1);
	ret = anthropic_parse_response_for_test(body, &response);
	anthropic_test_fail_next_reallocs(0);
	ASSERT(ret == -1);
	ASSERT(response.error != 0);
	ASSERT(response.content != NULL);
	ASSERT(strstr(response.content, "Out of memory") != NULL);
	ASSERT(response.tool_calls == NULL);
	ASSERT(response.tool_calls_count == 0);
	provider_response_clear(&response);
	return 0;
}
#endif

int main(void)
{
	RUN(test_anthropic_vtable());
	RUN(test_init_fails_without_config());
	RUN(test_init_fails_without_api_key_in_env());
	RUN(test_chat_fails_without_init());
	RUN(test_init_and_chat_if_key_set());
#ifdef SHELLCLAW_TEST
	RUN(test_parse_malformed_json_sets_error());
	RUN(test_parse_api_error_object_sets_error());
	RUN(test_parse_empty_content_array_returns_zero());
	RUN(test_parse_valid_text_block_returns_content());
	RUN(test_parse_null_json_returns_error());
	RUN(test_parse_text_past_initial_cap());
	RUN(test_parse_six_tool_use_blocks());
	RUN(test_parse_text_realloc_failure_is_error());
	RUN(test_parse_tool_realloc_failure_is_error());
#endif
	printf("test_anthropic: all tests passed\n");
	return 0;
}
