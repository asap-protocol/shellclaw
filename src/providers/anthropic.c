/**
 * @file anthropic.c
 * @brief Anthropic provider: Claude Messages API, tool_use blocks.
 *
 * API key from environment via config; never logged.
 */
#define _POSIX_C_SOURCE 200809L

#include "core/config.h"
#include "providers/provider.h"
#include "cJSON.h"
#include <curl/curl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define ANTHROPIC_URL "https://api.anthropic.com/v1/messages"
#define ANTHROPIC_VERSION "2023-06-01"
#define REQUEST_TIMEOUT_SEC 120
#define CONNECT_TIMEOUT_SEC 30
#define ANTHROPIC_TEXT_INIT_CAP 256
#define ANTHROPIC_TOOL_INIT_CAP 4

static char *s_anth_api_key;
static const config_t *s_anth_cfg;

#ifdef SHELLCLAW_TEST
static int s_fail_next_reallocs;

void anthropic_test_fail_next_reallocs(int n)
{
	s_fail_next_reallocs = n < 0 ? 0 : n;
}
#endif

static void *anth_realloc(void *ptr, size_t size)
{
#ifdef SHELLCLAW_TEST
	if (s_fail_next_reallocs > 0) {
		s_fail_next_reallocs--;
		return NULL;
	}
#endif
	return realloc(ptr, size);
}

static int parse_fail(cJSON *root, char *text, provider_tool_call_t *tool_calls,
                     size_t tool_count, provider_response_t *response, const char *msg)
{
	if (tool_calls) {
		for (size_t i = 0; i < tool_count; i++) {
			free(tool_calls[i].id);
			free(tool_calls[i].name);
			free(tool_calls[i].arguments);
		}
		free(tool_calls);
	}
	free(text);
	cJSON_Delete(root);
	provider_set_error(response, msg);
	return -1;
}

/* Cap is published only after realloc succeeds. Publishing first made
 * memcpy/index use a size the heap block did not have (#80). */
static int ensure_text_cap(char **text, size_t *text_cap, size_t need)
{
	while (*text_cap < need) {
		size_t new_cap = *text_cap ? *text_cap : ANTHROPIC_TEXT_INIT_CAP;
		char *grown;
		if (new_cap > SIZE_MAX / 2)
			return -1;
		new_cap *= 2;
		grown = anth_realloc(*text, new_cap);
		if (!grown)
			return -1;
		*text = grown;
		*text_cap = new_cap;
	}
	return 0;
}

static int ensure_tool_cap(provider_tool_call_t **tool_calls, size_t *tool_cap,
                           size_t tool_count)
{
	size_t new_cap;
	provider_tool_call_t *grown;
	if (tool_count < *tool_cap)
		return 0;
	new_cap = *tool_cap ? *tool_cap : ANTHROPIC_TOOL_INIT_CAP;
	if (*tool_cap) {
		if (new_cap > SIZE_MAX / (2u * sizeof(*grown)))
			return -1;
		new_cap *= 2;
	}
	if (new_cap > SIZE_MAX / sizeof(*grown))
		return -1;
	grown = anth_realloc(*tool_calls, new_cap * sizeof(*grown));
	if (!grown)
		return -1;
	*tool_calls = grown;
	*tool_cap = new_cap;
	return 0;
}

static int append_text_block(char **text, size_t *text_len, size_t *text_cap, const char *t)
{
	size_t tlen;
	size_t need;
	if (!t)
		return 0;
	tlen = strlen(t);
	if (tlen > SIZE_MAX - 1 || *text_len > SIZE_MAX - tlen - 1)
		return -1;
	need = *text_len + tlen + 1;
	if (ensure_text_cap(text, text_cap, need) != 0)
		return -1;
	memcpy(*text + *text_len, t, tlen + 1);
	*text_len += tlen;
	return 0;
}

static int append_tool_use(provider_tool_call_t **tool_calls, size_t *tool_count,
                           size_t *tool_cap, cJSON *block)
{
	provider_tool_call_t *tc;
	cJSON *id_item;
	cJSON *name_item;
	cJSON *input_item;
	if (ensure_tool_cap(tool_calls, tool_cap, *tool_count) != 0)
		return -1;
	tc = &(*tool_calls)[*tool_count];
	tc->id = NULL;
	tc->name = NULL;
	tc->arguments = NULL;
	id_item = cJSON_GetObjectItem(block, "id");
	name_item = cJSON_GetObjectItem(block, "name");
	input_item = cJSON_GetObjectItem(block, "input");
	if (cJSON_IsString(id_item))
		tc->id = provider_dup_str(id_item->valuestring);
	if (cJSON_IsString(name_item))
		tc->name = provider_dup_str(name_item->valuestring);
	if (input_item)
		tc->arguments = cJSON_PrintUnformatted(input_item);
	(*tool_count)++;
	return 0;
}

static int parse_response_body(const char *response_buf, provider_response_t *response)
{
	cJSON *root = cJSON_Parse(response_buf);
	cJSON *err_obj;
	cJSON *content;
	cJSON *block;
	size_t text_len = 0;
	size_t text_cap = ANTHROPIC_TEXT_INIT_CAP;
	size_t tool_cap = ANTHROPIC_TOOL_INIT_CAP;
	size_t tool_count = 0;
	char *text;
	provider_tool_call_t *tool_calls;
	if (!root) {
		provider_set_error(response, "Failed to parse Anthropic response JSON");
		return -1;
	}
	err_obj = cJSON_GetObjectItem(root, "error");
	if (cJSON_IsObject(err_obj)) {
		cJSON *msg = cJSON_GetObjectItem(err_obj, "message");
		const char *errmsg = cJSON_IsString(msg) ? msg->valuestring : "Anthropic API error";
		provider_set_error(response, errmsg);
		cJSON_Delete(root);
		return -1;
	}
	content = cJSON_GetObjectItem(root, "content");
	if (!cJSON_IsArray(content)) {
		cJSON_Delete(root);
		return 0;
	}
	text = malloc(text_cap);
	if (!text)
		return parse_fail(root, NULL, NULL, 0, response,
		                   "Out of memory growing Anthropic text buffer");
	text[0] = '\0';
	tool_calls = malloc(tool_cap * sizeof(*tool_calls));
	if (!tool_calls)
		return parse_fail(root, text, NULL, 0, response,
		                   "Out of memory growing Anthropic tool_use array");
	cJSON_ArrayForEach(block, content) {
		cJSON *type_item = cJSON_GetObjectItem(block, "type");
		const char *type = cJSON_IsString(type_item) ? type_item->valuestring : NULL;
		if (type && strcmp(type, "text") == 0) {
			cJSON *text_item = cJSON_GetObjectItem(block, "text");
			const char *t = cJSON_IsString(text_item) ? text_item->valuestring : "";
			if (append_text_block(&text, &text_len, &text_cap, t) != 0)
				return parse_fail(root, text, tool_calls, tool_count, response,
				                 "Out of memory growing Anthropic text buffer");
		} else if (type && strcmp(type, "tool_use") == 0) {
			if (append_tool_use(&tool_calls, &tool_count, &tool_cap, block) != 0)
				return parse_fail(root, text, tool_calls, tool_count, response,
				                 "Out of memory growing Anthropic tool_use array");
		}
	}
	cJSON_Delete(root);
	response->content = text;
	response->tool_calls = tool_calls;
	response->tool_calls_count = tool_count;
	return 0;
}

static int do_request(const char *body, provider_response_t *response)
{
	CURL *curl = curl_easy_init();
	if (!curl) {
		provider_set_error(response, "Failed to initialize curl");
		return -1;
	}
	size_t key_len = s_anth_api_key ? strlen(s_anth_api_key) : 0;
	size_t key_header_size = key_len + 16;
	char *key_header = malloc(key_header_size);
	if (!key_header) {
		curl_easy_cleanup(curl);
		provider_set_error(response, "Out of memory");
		return -1;
	}
	snprintf(key_header, key_header_size, "x-api-key: %s", s_anth_api_key ? s_anth_api_key : "");
	provider_curl_buf_t resp_buf = { .buf = malloc(PROVIDER_RESP_BUF_INIT), .len = 0, .cap = PROVIDER_RESP_BUF_INIT };
	if (!resp_buf.buf) {
		free(key_header);
		curl_easy_cleanup(curl);
		provider_set_error(response, "Out of memory");
		return -1;
	}
	resp_buf.buf[0] = '\0';
	struct curl_slist *headers = NULL;
	headers = curl_slist_append(headers, "Content-Type: application/json");
	headers = curl_slist_append(headers, "anthropic-version: " ANTHROPIC_VERSION);
	headers = curl_slist_append(headers, key_header);
	curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
	curl_easy_setopt(curl, CURLOPT_URL, ANTHROPIC_URL);
	curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, provider_write_cb);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp_buf);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, (long)REQUEST_TIMEOUT_SEC);
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, (long)CONNECT_TIMEOUT_SEC);
	CURLcode res = curl_easy_perform(curl);
	long code = 0;
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
	curl_slist_free_all(headers);
	free(key_header);
	curl_easy_cleanup(curl);
	if (res != CURLE_OK) {
		free(resp_buf.buf);
		provider_set_error(response, curl_easy_strerror(res));
		return -1;
	}
	if (code < 200 || code >= 300) {
		char errmsg[160];
		snprintf(errmsg, sizeof(errmsg), "Anthropic API HTTP %ld", code);
		free(resp_buf.buf);
		provider_set_error(response, errmsg);
		return -1;
	}
	int ret = parse_response_body(resp_buf.buf, response);
	free(resp_buf.buf);
	return ret;
}

static int build_and_send(const provider_message_t *messages, size_t message_count,
                          const provider_tool_def_t *tools, size_t tool_count,
                          provider_response_t *response)
{
	if (!s_anth_cfg) { provider_set_error(response, "Anthropic provider not initialized"); return -1; }
	cJSON *root = cJSON_CreateObject();
	if (!root) { provider_set_error(response, "Out of memory"); return -1; }
	const char *model = config_agent_model(s_anth_cfg);
	int max_tokens = config_agent_max_tokens(s_anth_cfg);
	if (!model) model = "claude-3-5-sonnet-20241022";
	if (max_tokens <= 0) max_tokens = 4096;
	cJSON_AddItemToObject(root, "model", cJSON_CreateString(model));
	cJSON_AddItemToObject(root, "max_tokens", cJSON_CreateNumber(max_tokens));
	for (size_t i = 0; i < message_count; i++) {
		if (messages[i].role && strcmp(messages[i].role, "system") == 0) {
			cJSON_AddItemToObject(root, "system", cJSON_CreateString(messages[i].content ? messages[i].content : ""));
			break;
		}
	}
	cJSON *msg_arr = cJSON_CreateArray();
	if (!msg_arr) { cJSON_Delete(root); provider_set_error(response, "Out of memory"); return -1; }
	for (size_t i = 0; i < message_count; i++) {
		const char *role = messages[i].role ? messages[i].role : "user";
		if (strcmp(role, "system") == 0) continue;
		cJSON *msg = cJSON_CreateObject();
		if (!msg) {
			cJSON_Delete(root);
			provider_set_error(response, "Out of memory");
			return -1;
		}
		cJSON_AddItemToObject(msg, "role", cJSON_CreateString(role));
		if (messages[i].tool_use_id) {
			cJSON *content_arr = cJSON_CreateArray();
			if (content_arr) {
				cJSON *tr = cJSON_CreateObject();
				if (tr) {
					cJSON_AddItemToObject(tr, "type", cJSON_CreateString("tool_result"));
					cJSON_AddItemToObject(tr, "tool_use_id", cJSON_CreateString(messages[i].tool_use_id));
					cJSON_AddItemToObject(tr, "content", cJSON_CreateString(messages[i].content ? messages[i].content : ""));
					cJSON_AddItemToArray(content_arr, tr);
				}
				cJSON_AddItemToObject(msg, "content", content_arr);
			} else
				cJSON_AddItemToObject(msg, "content", cJSON_CreateString(""));
		} else if (messages[i].tool_calls && messages[i].tool_calls_count > 0 && strcmp(role, "assistant") == 0) {
			cJSON *content_arr = cJSON_CreateArray();
			if (content_arr) {
				if (messages[i].content && messages[i].content[0] != '\0') {
					cJSON *text_block = cJSON_CreateObject();
					if (text_block) {
						cJSON_AddItemToObject(text_block, "type", cJSON_CreateString("text"));
						cJSON_AddItemToObject(text_block, "text", cJSON_CreateString(messages[i].content));
						cJSON_AddItemToArray(content_arr, text_block);
					}
				}
				for (size_t k = 0; k < messages[i].tool_calls_count; k++) {
					const provider_tool_call_t *tc = &messages[i].tool_calls[k];
					cJSON *tu = cJSON_CreateObject();
					if (!tu) break;
					cJSON_AddItemToObject(tu, "type", cJSON_CreateString("tool_use"));
					cJSON_AddItemToObject(tu, "id", cJSON_CreateString(tc->id ? tc->id : ""));
					cJSON_AddItemToObject(tu, "name", cJSON_CreateString(tc->name ? tc->name : ""));
					cJSON *input = tc->arguments && tc->arguments[0] ? cJSON_Parse(tc->arguments) : cJSON_CreateObject();
					if (input) cJSON_AddItemToObject(tu, "input", input);
					cJSON_AddItemToArray(content_arr, tu);
				}
				cJSON_AddItemToObject(msg, "content", content_arr);
			} else
				cJSON_AddItemToObject(msg, "content", cJSON_CreateString(messages[i].content ? messages[i].content : ""));
		} else
			cJSON_AddItemToObject(msg, "content", cJSON_CreateString(messages[i].content ? messages[i].content : ""));
		cJSON_AddItemToArray(msg_arr, msg);
	}
	cJSON_AddItemToObject(root, "messages", msg_arr);
	if (tool_count > 0 && tools) {
		cJSON *tools_arr = cJSON_CreateArray();
		if (tools_arr) {
			for (size_t i = 0; i < tool_count; i++) {
				cJSON *t = cJSON_CreateObject();
				if (!t) break;
				cJSON_AddItemToObject(t, "name", cJSON_CreateString(tools[i].name ? tools[i].name : ""));
				cJSON_AddItemToObject(t, "description", cJSON_CreateString(tools[i].description ? tools[i].description : ""));
				if (tools[i].parameters_json && tools[i].parameters_json[0]) {
					cJSON *schema = cJSON_Parse(tools[i].parameters_json);
					if (schema) {
						cJSON_AddItemToObject(t, "input_schema", schema);
					}
				}
				cJSON_AddItemToArray(tools_arr, t);
			}
			cJSON_AddItemToObject(root, "tools", tools_arr);
		}
	}
	char *body = cJSON_PrintUnformatted(root);
	cJSON_Delete(root);
	if (!body) { provider_set_error(response, "Out of memory"); return -1; }
	int ret = do_request(body, response);
	cJSON_free(body);
	return ret;
}

static int anthropic_init(const config_t *cfg)
{
	if (!cfg) return -1;
	const char *env_name = config_provider_anthropic_api_key_env(cfg);
	if (!env_name || !env_name[0]) return -1;
	const char *key = getenv(env_name);
	if (!key || !key[0]) return -1;
	free(s_anth_api_key);
	s_anth_api_key = strdup(key);
	s_anth_cfg = s_anth_api_key ? cfg : NULL;
	return s_anth_api_key ? 0 : -1;
}

static int anthropic_chat(const provider_message_t *messages, size_t message_count,
                          const provider_tool_def_t *tools, size_t tool_count,
                          provider_response_t *response)
{
	if (!s_anth_api_key || !s_anth_cfg) {
		provider_set_error(response, "Anthropic provider not initialized or API key missing");
		return -1;
	}
	provider_response_clear(response);
	return build_and_send(messages, message_count, tools, tool_count, response);
}

static void anthropic_cleanup(void)
{
	if (s_anth_api_key) {
		volatile char *p = (volatile char *)s_anth_api_key;
		for (size_t i = 0; s_anth_api_key[i] != '\0'; i++) p[i] = '\0';
		free(s_anth_api_key);
		s_anth_api_key = NULL;
	}
	s_anth_cfg = NULL;
}

static const provider_t anthropic_provider = {
	.name = "anthropic",
	.init = anthropic_init,
	.chat = anthropic_chat,
	.cleanup = anthropic_cleanup,
};

const provider_t *provider_anthropic_get(void)
{
	return &anthropic_provider;
}

#ifdef SHELLCLAW_TEST
int anthropic_parse_response_for_test(const char *json, provider_response_t *response)
{
	if (!json || !response) return -1;
	return parse_response_body(json, response);
}
#endif
