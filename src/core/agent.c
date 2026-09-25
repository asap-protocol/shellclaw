/**
 * @file agent.c
 * @brief ReAct agent loop implementation.
 */
#define _POSIX_C_SOURCE 200809L

#include "core/agent.h"
#include "core/config.h"
#include "core/memory.h"
#include "core/skill.h"
#include "providers/provider.h"
#include "cJSON.h"
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static pthread_mutex_t g_agent_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Test override: non-empty string replaces router snapshot for local/offline prompt suffix. */
static const char *g_agent_test_active_backend_override;

void shellclaw_agent_set_test_active_backend_name(const char *name_or_null)
{
	g_agent_test_active_backend_override = name_or_null;
}

void agent_lock(void)
{
	pthread_mutex_lock(&g_agent_mutex);
}

void agent_unlock(void)
{
	pthread_mutex_unlock(&g_agent_mutex);
}

int agent_mutex_is_locked_for_test(void)
{
	int rc = pthread_mutex_trylock(&g_agent_mutex);
	if (rc == 0) {
		pthread_mutex_unlock(&g_agent_mutex);
		return 0;
	}
	return 1;
}

#define SYSTEM_PROMPT_MAX      65536
#define SKILLS_BUF_SIZE        32768
#define SESSION_JSON_MAX       (128 * 1024)
#define RECALL_BUF_SIZE        8192
#define RECALL_LIMIT           5
#define HISTORY_CONTENT_MAX    (128 * 1024)
#define MAX_HISTORY_MESSAGES   40
#define ROLE_LEN               16
#define MAX_TOOL_CALLS         16
#define TOOL_RESULT_SIZE       4096
#define SUMMARY_SOURCE_MAX     (64 * 1024)
#define SUMMARY_RESULT_MAX     4096

/** Copy cJSON_PrintUnformatted output or refuse mid-JSON truncation (Refs: #70). */
static int copy_printed_session_json(char *dst, size_t dst_size, char *printed,
	const char *session_id)
{
	size_t len;
	if (!printed)
		return -1;
	len = strlen(printed);
	if (len >= dst_size) {
		fprintf(stderr,
			"agent: refuse session JSON truncation session_id=%s len=%zu cap=%zu\n",
			session_id ? session_id : "", len, dst_size);
		cJSON_free(printed);
		return -1;
	}
	memcpy(dst, printed, len + 1);
	cJSON_free(printed);
	return 0;
}

static const char SUMMARIZE_SYSTEM[] = "Summarize the following conversation in one short paragraph. Output only the summary, no preamble.";

/** Summarize oldest messages when over max_ctx; replace with one summary + trailing. */
static int compact_session_via_llm(const char *session_id, char *session_buf, size_t session_buf_size,
	int msg_count, int max_ctx, const provider_t *provider)
{
	if (msg_count <= max_ctx || max_ctx < 2) return 0;
	cJSON *root = cJSON_Parse(session_buf);
	if (!root || !cJSON_IsArray(root)) {
		if (root) cJSON_Delete(root);
		return -1;
	}
	/* Tail keeps max_ctx-1 messages, so the summary must include index
	 * msg_count-max_ctx. Stopping one short drops that turn from SQLite. */
	int to_summarize_n = msg_count - max_ctx + 1;
	char *source_buf = malloc(SUMMARY_SOURCE_MAX);
	if (!source_buf) { cJSON_Delete(root); return -1; }
	source_buf[0] = '\0';
	size_t used = 0;
	for (int i = 0; i < to_summarize_n && used < SUMMARY_SOURCE_MAX - 1; i++) {
		const cJSON *item = cJSON_GetArrayItem(root, i);
		if (!item || !cJSON_IsObject(item)) continue;
		const char *role = "user";
		const char *content = "";
		const cJSON *r = cJSON_GetObjectItem(item, "role");
		const cJSON *c = cJSON_GetObjectItem(item, "content");
		if (cJSON_IsString(r)) role = r->valuestring;
		if (cJSON_IsString(c)) content = c->valuestring;
		int n = snprintf(source_buf + used, SUMMARY_SOURCE_MAX - used, "%s: %s\n", role, content ? content : "");
		if (n > 0 && (size_t)n < SUMMARY_SOURCE_MAX - used) used += (size_t)n;
	}
	provider_message_t summ_msgs[2] = {
		{ .role = "system", .content = SUMMARIZE_SYSTEM, .tool_calls = NULL, .tool_calls_count = 0, .tool_use_id = NULL },
		{ .role = "user", .content = source_buf, .tool_calls = NULL, .tool_calls_count = 0, .tool_use_id = NULL },
	};
	provider_response_t resp = {0};
	int err = provider->chat(summ_msgs, 2, NULL, 0, &resp);
	free(source_buf);
	if (err != 0 || !resp.content) {
		provider_response_clear(&resp);
		cJSON_Delete(root);
		return -1;
	}
	char summary_buf[SUMMARY_RESULT_MAX];
	size_t sum_len = strlen(resp.content);
	if (sum_len >= SUMMARY_RESULT_MAX) sum_len = SUMMARY_RESULT_MAX - 1;
	memcpy(summary_buf, resp.content, sum_len);
	summary_buf[sum_len] = '\0';
	provider_response_clear(&resp);
	cJSON *new_arr = cJSON_CreateArray();
	if (!new_arr) { cJSON_Delete(root); return -1; }
	cJSON *summ_obj = cJSON_CreateObject();
	if (summ_obj) {
		cJSON_AddItemToObject(summ_obj, "role", cJSON_CreateString("user"));
		cJSON_AddItemToObject(summ_obj, "content", cJSON_CreateString(summary_buf));
		cJSON_AddItemToArray(new_arr, summ_obj);
	}
	int keep = max_ctx - 1;
	int start = msg_count - keep;
	if (start < 0) start = 0;
	for (int i = start; i < msg_count; i++) {
		const cJSON *item = cJSON_GetArrayItem(root, i);
		if (item) {
			cJSON *dup = cJSON_Duplicate(item, 1);
			if (dup) cJSON_AddItemToArray(new_arr, dup);
		}
	}
	cJSON_Delete(root);
	char *printed = cJSON_PrintUnformatted(new_arr);
	cJSON_Delete(new_arr);
	if (copy_printed_session_json(session_buf, session_buf_size, printed, session_id) != 0)
		return -1;
	session_save(session_id, session_buf);
	return 0;
}

/** Fallback when SOUL+IDENTITY+skills are all empty. */
static const char SYSTEM_PROMPT_FALLBACK[] = "You are a helpful assistant.";

/** Appended when active backend name is `local` (router snapshot or test override). */
static const char AGENT_LOCAL_OFFLINE_NOTE[] =
	"\n\n[Operating mode: local/offline inference]\n"
	"You are served by a local model; capabilities may be reduced compared to larger cloud models. "
	"When relevant, acknowledge limits, avoid claiming access or tools you do not have, and prefer concise, accurate answers.";

static int agent_active_backend_is_local_name(void)
{
	char buf[96];
	const char *p;
	if (g_agent_test_active_backend_override && g_agent_test_active_backend_override[0] != '\0')
		p = g_agent_test_active_backend_override;
	else {
		provider_router_active_backend_snapshot(buf, sizeof(buf));
		p = buf;
	}
	return p[0] != '\0' && strcasecmp(p, "local") == 0;
}

static void append_local_offline_note_if_needed(char *system_buf, size_t buf_size)
{
	size_t len;
	size_t note_len;
	if (!agent_active_backend_is_local_name())
		return;
	len = strlen(system_buf);
	note_len = strlen(AGENT_LOCAL_OFFLINE_NOTE);
	if (len >= buf_size)
		return;
	if (len + note_len + 1U > buf_size) {
		size_t copy_len = buf_size - len - 1U;
		if (copy_len == 0U)
			return;
		memcpy(system_buf + len, AGENT_LOCAL_OFFLINE_NOTE, copy_len);
		system_buf[len + copy_len] = '\0';
		return;
	}
	memcpy(system_buf + len, AGENT_LOCAL_OFFLINE_NOTE, note_len + 1U);
}

static void copy_response_to_buf(const char *content, char *response_buf, size_t response_size)
{
	if (!response_buf || response_size == 0) return;
	if (content) {
		size_t n = strlen(content);
		if (n >= response_size) n = response_size - 1;
		memcpy(response_buf, content, n);
		response_buf[n] = '\0';
	} else
		response_buf[0] = '\0';
}

static size_t append_memories_to_system(char *system_buf, size_t buf_size, const char *recall_buf)
{
	size_t len = strlen(system_buf);
	size_t prefix_len;
	size_t recall_len;
	size_t remain;
	const char *prefix = "\n\nRelevant memories:\n\n";
	if (len == 0 || !recall_buf || recall_buf[0] == '\0')
		return len;
	/* prefix_len is never 0, so the old `prefix_len + recall_len == 0` guard
	 * never fired. When the prompt filled SYSTEM_PROMPT_MAX, recall_len was
	 * clamped to 0 and memcpy still wrote the prefix (and a NUL) past the heap
	 * buffer (Refs: #75). Skip unless prefix + at least one recall byte + NUL fit. */
	prefix_len = strlen(prefix);
	if (len >= buf_size || buf_size - len < prefix_len + 2U) {
		fprintf(stderr, "agent: skip memory injection len=%zu cap=%zu\n", len, buf_size);
		return len;
	}
	recall_len = strlen(recall_buf);
	remain = buf_size - len - prefix_len - 1U;
	if (recall_len > remain)
		recall_len = remain;
	if (recall_len == 0)
		return len;
	memcpy(system_buf + len, prefix, prefix_len);
	len += prefix_len;
	memcpy(system_buf + len, recall_buf, recall_len);
	len += recall_len;
	system_buf[len] = '\0';
	return len;
}

/** Parse session JSON into message slots; copy roles and content into given buffers. */
static int parse_session_into_messages(const char *session_json, int max_messages,
	provider_message_t *out_msgs, char *role_buf, size_t role_buf_size,
	char *content_buf, size_t content_buf_size, int *out_count)
{
	*out_count = 0;
	if (!session_json || session_json[0] != '[') return 0;
	cJSON *root = cJSON_Parse(session_json);
	if (!root || !cJSON_IsArray(root)) {
		if (root) cJSON_Delete(root);
		return 0;
	}
	int arr_len = cJSON_GetArraySize(root);
	int skip = arr_len > max_messages ? arr_len - max_messages : 0;
	size_t content_used = 0;
	for (int i = skip; i < arr_len && *out_count < max_messages; i++) {
		cJSON *item = cJSON_GetArrayItem(root, i);
		if (!item || !cJSON_IsObject(item)) continue;
		cJSON *role_item = cJSON_GetObjectItem(item, "role");
		cJSON *content_item = cJSON_GetObjectItem(item, "content");
		const char *role_s = role_item && cJSON_IsString(role_item) ? role_item->valuestring : "user";
		const char *content_s = content_item && cJSON_IsString(content_item) ? content_item->valuestring : "";
		size_t rlen = strlen(role_s);
		size_t clen = strlen(content_s);
		size_t role_off = (size_t)(*out_count) * ROLE_LEN;
		if (rlen >= ROLE_LEN || role_off + ROLE_LEN > role_buf_size) break;
		if (content_used + clen + 1 > content_buf_size) break;
		memcpy(role_buf + role_off, role_s, rlen + 1);
		memcpy(content_buf + content_used, content_s, clen + 1);
		out_msgs[*out_count].role = role_buf + role_off;
		out_msgs[*out_count].content = content_buf + content_used;
		content_used += clen + 1;
		(*out_count)++;
	}
	cJSON_Delete(root);
	return 0;
}

static const agent_tool_t *find_tool(const agent_tool_t *tools, size_t tool_count, const char *name)
{
	if (!tools || !name) return NULL;
	for (size_t i = 0; i < tool_count; i++) {
		if (tools[i].name && strcmp(tools[i].name, name) == 0)
			return &tools[i];
	}
	return NULL;
}

static void agent_free_owned_ptr(const void *p)
{
	free((void *)(uintptr_t)p);
}

static void free_tool_calls_copy(const provider_tool_call_t *copy, size_t n)
{
	provider_tool_call_t *owned;
	size_t i;
	if (!copy) return;
	owned = (provider_tool_call_t *)(uintptr_t)copy;
	for (i = 0; i < n; i++) {
		free(owned[i].id);
		free(owned[i].name);
		free(owned[i].arguments);
	}
	free(owned);
}

/** Append user+assistant exchange to session JSON. Invalid or empty existing becomes []. */
static int append_exchange_to_session_json(const char *existing_json, const char *user_message,
	const char *assistant_content, char *out_buf, size_t out_size, const char *session_id)
{
	cJSON *arr = NULL;
	if (existing_json && existing_json[0] == '[') {
		arr = cJSON_Parse(existing_json);
		if (arr && !cJSON_IsArray(arr)) {
			cJSON_Delete(arr);
			arr = NULL;
		}
	}
	if (!arr) arr = cJSON_CreateArray();
	if (!arr) return -1;
	cJSON *user_obj = cJSON_CreateObject();
	if (user_obj) {
		cJSON_AddItemToObject(user_obj, "role", cJSON_CreateString("user"));
		cJSON_AddItemToObject(user_obj, "content", cJSON_CreateString(user_message ? user_message : ""));
		cJSON_AddItemToArray(arr, user_obj);
	}
	cJSON *asst_obj = cJSON_CreateObject();
	if (asst_obj) {
		cJSON_AddItemToObject(asst_obj, "role", cJSON_CreateString("assistant"));
		cJSON_AddItemToObject(asst_obj, "content", cJSON_CreateString(assistant_content ? assistant_content : ""));
		cJSON_AddItemToArray(arr, asst_obj);
	}
	char *printed = cJSON_PrintUnformatted(arr);
	cJSON_Delete(arr);
	return copy_printed_session_json(out_buf, out_size, printed, session_id);
}

static provider_tool_call_t *copy_tool_calls(const provider_tool_call_t *src, size_t n)
{
	if (!src || n == 0) return NULL;
	provider_tool_call_t *dst = malloc(n * sizeof(provider_tool_call_t));
	if (!dst) return NULL;
	for (size_t i = 0; i < n; i++) {
		dst[i].id = src[i].id ? strdup(src[i].id) : NULL;
		dst[i].name = src[i].name ? strdup(src[i].name) : NULL;
		dst[i].arguments = src[i].arguments ? strdup(src[i].arguments) : NULL;
		if ((src[i].id && !dst[i].id) || (src[i].name && !dst[i].name) || (src[i].arguments && !dst[i].arguments)) {
			free_tool_calls_copy(dst, i + 1);
			return NULL;
		}
	}
	return dst;
}

typedef struct agent_run_ctx {
	const config_t *cfg;
	const char *session_id;
	const char *user_message;
	const provider_t *provider;
	const agent_tool_t *tools;
	size_t tool_count;
	char *response_buf;
	size_t response_size;
	provider_tool_def_t *tool_defs;
	char *system_buf;
	char *skills_buf;
	char *session_buf;
	char *recall_buf;
	char *history_content;
	provider_message_t *history_msgs;
	char *history_roles_buf;
	char *tool_result_bufs;
	provider_message_t *messages;
	size_t total_msgs;
	size_t base_msg_count;
	int history_count;
	int max_iter;
	int max_ctx;
	int ret;
	int skip_session_persist;
} agent_run_ctx_t;

static void agent_oom_msg(agent_run_ctx_t *ctx)
{
	if (ctx->response_buf && ctx->response_size > 0) {
		strncpy(ctx->response_buf, "agent_run: out of memory", ctx->response_size - 1);
		ctx->response_buf[ctx->response_size - 1] = '\0';
	}
}

/** Free ReAct-owned slots (assistant text, tool results, tool_calls copies). */
static void agent_free_heap_messages(agent_run_ctx_t *ctx, size_t from_idx)
{
	size_t i;
	if (!ctx->messages) return;
	/* tool_use_id aliases our_calls[k].id; drop it before freeing tool_calls. */
	for (i = from_idx; i < ctx->total_msgs; i++)
		ctx->messages[i].tool_use_id = NULL;
	for (i = from_idx; i < ctx->total_msgs; i++) {
		agent_free_owned_ptr(ctx->messages[i].content);
		ctx->messages[i].content = NULL;
		free_tool_calls_copy(ctx->messages[i].tool_calls, ctx->messages[i].tool_calls_count);
		ctx->messages[i].tool_calls = NULL;
		ctx->messages[i].tool_calls_count = 0;
	}
}

/** Load skills, system prompt, memories, session history; compact if over limit. */
static int agent_prepare_context(agent_run_ctx_t *ctx)
{
	cJSON *parsed;
	int msg_count;
	ctx->system_buf = malloc(SYSTEM_PROMPT_MAX);
	ctx->skills_buf = malloc(SKILLS_BUF_SIZE);
	ctx->session_buf = malloc(SESSION_JSON_MAX);
	ctx->recall_buf = malloc(RECALL_BUF_SIZE);
	ctx->history_content = malloc(HISTORY_CONTENT_MAX);
	if (!ctx->system_buf || !ctx->skills_buf || !ctx->session_buf || !ctx->recall_buf ||
	    !ctx->history_content) {
		agent_oom_msg(ctx);
		return -1;
	}
	ctx->history_msgs = malloc(MAX_HISTORY_MESSAGES * sizeof(provider_message_t));
	ctx->history_roles_buf = malloc(MAX_HISTORY_MESSAGES * ROLE_LEN);
	ctx->tool_result_bufs = malloc((size_t)MAX_TOOL_CALLS * TOOL_RESULT_SIZE);
	if (!ctx->history_msgs || !ctx->history_roles_buf || !ctx->tool_result_bufs) {
		agent_oom_msg(ctx);
		return -1;
	}
	skill_load_all(ctx->cfg, ctx->skills_buf, SKILLS_BUF_SIZE);
	if (skill_build_system_prompt_base(ctx->cfg, ctx->skills_buf, ctx->system_buf,
	                                   SYSTEM_PROMPT_MAX) != 0) {
		strncpy(ctx->system_buf, SYSTEM_PROMPT_FALLBACK, SYSTEM_PROMPT_MAX - 1);
		ctx->system_buf[SYSTEM_PROMPT_MAX - 1] = '\0';
	}
	ctx->recall_buf[0] = '\0';
	memory_recall(ctx->user_message, ctx->recall_buf, RECALL_BUF_SIZE, RECALL_LIMIT);
	append_memories_to_system(ctx->system_buf, SYSTEM_PROMPT_MAX, ctx->recall_buf);
	append_local_offline_note_if_needed(ctx->system_buf, SYSTEM_PROMPT_MAX);
	ctx->max_ctx = config_agent_max_context_messages(ctx->cfg);
	if (ctx->max_ctx <= 0 || ctx->max_ctx > MAX_HISTORY_MESSAGES)
		ctx->max_ctx = MAX_HISTORY_MESSAGES;
	ctx->session_buf[0] = '\0';
	{
		int load_rc = session_load(ctx->session_id, ctx->session_buf, SESSION_JSON_MAX);
		ctx->skip_session_persist = (load_rc == SESSION_LOAD_TOO_LARGE);
	}
	parsed = cJSON_Parse(ctx->session_buf);
	msg_count = (parsed && cJSON_IsArray(parsed)) ? cJSON_GetArraySize(parsed) : 0;
	if (parsed)
		cJSON_Delete(parsed);
	if (msg_count > ctx->max_ctx)
		compact_session_via_llm(ctx->session_id, ctx->session_buf, SESSION_JSON_MAX, msg_count,
		                        ctx->max_ctx, ctx->provider);
	ctx->max_iter = config_agent_max_tool_iterations(ctx->cfg);
	if (ctx->max_iter <= 0)
		ctx->max_iter = 20;
	return 0;
}

/** Build provider message array: system + history + current user turn. */
static int agent_build_messages(agent_run_ctx_t *ctx)
{
	ctx->history_count = 0;
	parse_session_into_messages(ctx->session_buf, ctx->max_ctx, ctx->history_msgs,
	                            ctx->history_roles_buf, MAX_HISTORY_MESSAGES * ROLE_LEN,
	                            ctx->history_content, HISTORY_CONTENT_MAX, &ctx->history_count);
	ctx->total_msgs = 1 + (size_t)ctx->history_count + 1;
	ctx->messages = malloc(ctx->total_msgs * sizeof(provider_message_t));
	if (!ctx->messages) {
		agent_oom_msg(ctx);
		return -1;
	}
	memset(ctx->messages, 0, ctx->total_msgs * sizeof(provider_message_t));
	ctx->messages[0].role = "system";
	ctx->messages[0].content = ctx->system_buf;
	for (int i = 0; i < ctx->history_count; i++) {
		ctx->messages[1 + i].role = ctx->history_msgs[i].role;
		ctx->messages[1 + i].content = ctx->history_msgs[i].content;
	}
	ctx->messages[1 + ctx->history_count].role = "user";
	ctx->messages[1 + ctx->history_count].content = ctx->user_message;
	ctx->base_msg_count = ctx->total_msgs;
	return 0;
}

static void agent_persist_session(agent_run_ctx_t *ctx, const char *assistant_content)
{
	char *updated;
	if (ctx->skip_session_persist) {
		fprintf(stderr,
			"agent: skip session persist session_id=%s (stored blob exceeds cap)\n",
			ctx->session_id ? ctx->session_id : "");
		return;
	}
	updated = malloc(SESSION_JSON_MAX);
	if (!updated)
		return;
	if (append_exchange_to_session_json(ctx->session_buf, ctx->user_message, assistant_content,
	                                    updated, SESSION_JSON_MAX, ctx->session_id) == 0)
		session_save(ctx->session_id, updated);
	free(updated);
}

/** ReAct loop: LLM chat, tool execution, message growth until done or max iterations. */
static int agent_react_loop(agent_run_ctx_t *ctx)
{
	int iteration = 0;
	provider_response_t response = {0};
	for (;;) {
		int err = ctx->provider->chat(ctx->messages, ctx->total_msgs, ctx->tool_defs, ctx->tool_count,
		                              &response);
		if (err != 0) {
			copy_response_to_buf(response.content, ctx->response_buf, ctx->response_size);
			provider_response_clear(&response);
			return -1;
		}
		if (response.tool_calls_count == 0 || iteration >= ctx->max_iter) {
			copy_response_to_buf(response.content, ctx->response_buf, ctx->response_size);
			provider_response_clear(&response);
			agent_persist_session(ctx, ctx->response_buf);
			return 0;
		}
		{
			size_t nc = response.tool_calls_count;
			char *assistant_content;
			provider_tool_call_t *our_calls;
			provider_message_t *new_messages;
			size_t new_count;
			if (nc > MAX_TOOL_CALLS)
				nc = MAX_TOOL_CALLS;
			assistant_content = response.content ? strdup(response.content) : strdup("");
			if (!assistant_content) {
				provider_response_clear(&response);
				agent_oom_msg(ctx);
				return -1;
			}
			our_calls = copy_tool_calls(response.tool_calls, nc);
			provider_response_clear(&response);
			if (!our_calls) {
				free(assistant_content);
				agent_oom_msg(ctx);
				return -1;
			}
			for (size_t k = 0; k < nc; k++) {
				char *one_buf = ctx->tool_result_bufs + k * TOOL_RESULT_SIZE;
				const agent_tool_t *tool = find_tool(ctx->tools, ctx->tool_count, our_calls[k].name);
				one_buf[0] = '\0';
				if (tool && tool->execute)
					tool->execute(our_calls[k].arguments ? our_calls[k].arguments : "{}", one_buf,
					              TOOL_RESULT_SIZE);
				else
					snprintf(one_buf, TOOL_RESULT_SIZE, "error: unknown tool \"%s\"",
					         our_calls[k].name ? our_calls[k].name : "");
			}
			new_count = ctx->total_msgs + 1 + nc;
			new_messages = malloc(new_count * sizeof(provider_message_t));
			if (!new_messages) {
				free_tool_calls_copy(our_calls, nc);
				free(assistant_content);
				agent_oom_msg(ctx);
				return -1;
			}
			memset(new_messages, 0, new_count * sizeof(provider_message_t));
			memcpy(new_messages, ctx->messages, ctx->total_msgs * sizeof(provider_message_t));
			new_messages[ctx->total_msgs].role = "assistant";
			new_messages[ctx->total_msgs].content = assistant_content;
			new_messages[ctx->total_msgs].tool_calls = our_calls;
			new_messages[ctx->total_msgs].tool_calls_count = nc;
			for (size_t k = 0; k < nc; k++) {
				char *one_buf = ctx->tool_result_bufs + k * TOOL_RESULT_SIZE;
				/* Scratch is reused each round; history must own a copy (Refs: #59). */
				char *tool_content = strdup(one_buf);
				if (!tool_content) {
					for (size_t j = 0; j < k; j++)
						agent_free_owned_ptr(new_messages[ctx->total_msgs + 1 + j].content);
					free(new_messages);
					free_tool_calls_copy(our_calls, nc);
					free(assistant_content);
					agent_oom_msg(ctx);
					return -1;
				}
				new_messages[ctx->total_msgs + 1 + k].role = "user";
				new_messages[ctx->total_msgs + 1 + k].content = tool_content;
				new_messages[ctx->total_msgs + 1 + k].tool_use_id = our_calls[k].id;
			}
			free(ctx->messages);
			ctx->messages = new_messages;
			ctx->total_msgs = new_count;
			iteration++;
		}
	}
}

static void agent_run_cleanup(agent_run_ctx_t *ctx)
{
	if (ctx->messages && ctx->base_msg_count < ctx->total_msgs)
		agent_free_heap_messages(ctx, ctx->base_msg_count);
	free(ctx->system_buf);
	free(ctx->skills_buf);
	free(ctx->session_buf);
	free(ctx->recall_buf);
	free(ctx->history_content);
	free(ctx->tool_defs);
	free(ctx->history_msgs);
	free(ctx->history_roles_buf);
	free(ctx->tool_result_bufs);
	free(ctx->messages);
}

int agent_run(const config_t *cfg, const char *session_id, const char *user_message,
              const provider_t *provider, const agent_tool_t *tools, size_t tool_count,
              char *response_buf, size_t response_size)
{
	agent_run_ctx_t ctx = {0};
	size_t i;
	if (!cfg || !session_id || !user_message || !provider || !response_buf || response_size == 0) {
		if (response_buf && response_size > 0)
			strncpy(response_buf, "agent_run: invalid arguments", response_size - 1);
		if (response_buf && response_size > 0)
			response_buf[response_size - 1] = '\0';
		return -1;
	}
	ctx.cfg = cfg;
	ctx.session_id = session_id;
	ctx.user_message = user_message;
	ctx.provider = provider;
	ctx.tools = tools;
	ctx.tool_count = tool_count;
	ctx.response_buf = response_buf;
	ctx.response_size = response_size;
	ctx.ret = -1;
	if (tool_count > 0 && tools) {
		ctx.tool_defs = malloc(tool_count * sizeof(provider_tool_def_t));
		if (!ctx.tool_defs) {
			agent_oom_msg(&ctx);
			return -1;
		}
		for (i = 0; i < tool_count; i++) {
			ctx.tool_defs[i].name = tools[i].name;
			ctx.tool_defs[i].description = tools[i].description;
			ctx.tool_defs[i].parameters_json = tools[i].parameters_json;
		}
	}
	if (agent_prepare_context(&ctx) != 0)
		goto cleanup;
	if (agent_build_messages(&ctx) != 0)
		goto cleanup;
	ctx.ret = agent_react_loop(&ctx);
cleanup:
	agent_run_cleanup(&ctx);
	return ctx.ret;
}
