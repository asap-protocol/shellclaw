/**
 * @file server.c
 * @brief Inbound ASAP dispatch by payload_type (task.request, state.query, mcp.tool_call, …).
 */
#define _POSIX_C_SOURCE 200809L

#include "asap/server.h"
#include "asap/asap_version.h"
#include "asap/ulid.h"
#include "core/agent.h"
#include "core/config.h"
#include "core/memory.h"
#include "providers/provider.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

enum { ASAP_SERVER_AGENT_RESPONSE_CAP = 256 * 1024 };
enum { ASAP_TASK_SESSION_ID_CAP = 512 };

static void set_err(char *buf, size_t sz, const char *msg)
{
	if (!buf || sz == 0) return;
	if (!msg) msg = "error";
	snprintf(buf, sz, "%s", msg);
	buf[sz - 1] = '\0';
}

static int sender_is_trusted(const config_t *cfg, const char *sender)
{
	int i;
	int n;
	if (!cfg) return 1;
	n = config_asap_trusted_senders_count(cfg);
	if (n <= 0) return 1;
	if (!sender || sender[0] == '\0') return 0;
	for (i = 0; i < n; i++) {
		const char *t = config_asap_trusted_sender(cfg, i);
		if (t && strcmp(t, sender) == 0) return 1;
	}
	return 0;
}

static const char *payload_pick_string(const cJSON *payload, const char *key)
{
	const cJSON *j = cJSON_GetObjectItemCaseSensitive(payload, key);
	if (!j || !cJSON_IsString(j) || !j->valuestring) return NULL;
	return j->valuestring;
}

static char *task_payload_to_prompt(const cJSON *payload)
{
	const char *s;
	char *printed;
	if (!payload) return NULL;
	s = payload_pick_string(payload, "input");
	if (!s) s = payload_pick_string(payload, "prompt");
	if (!s) s = payload_pick_string(payload, "message");
	if (s) return strdup(s);
	printed = cJSON_PrintUnformatted((cJSON *)payload);
	return printed;
}

/** @return 0 success, 1 tool not found, 2 execute returned non-zero */
static int dispatch_tool_by_name(const asap_server_ctx_t *ctx, const char *name,
	const char *args_json, char *result_buf, size_t max_len)
{
	size_t i;
	if (!ctx || !name || !args_json || !result_buf || max_len == 0) return 2;
	result_buf[0] = '\0';
	for (i = 0; i < ctx->tool_count; i++) {
		const agent_tool_t *t = &ctx->tools[i];
		int er;
		if (!t->name || strcmp(t->name, name) != 0) continue;
		if (!t->execute) return 2;
		er = t->execute(args_json, result_buf, max_len);
		return er == 0 ? 0 : 2;
	}
	return 1;
}

static int fill_response_envelope(asap_envelope_t *out, const asap_envelope_t *in,
	const char *payload_type, cJSON *payload)
{
	char ulid_buf[ULID_STRING_LEN + 1];
	if (!out || !in || !payload_type || !payload) {
		if (payload) cJSON_Delete(payload);
		return -32603;
	}
	if (ulid_generate(ulid_buf, sizeof ulid_buf) != 0) {
		cJSON_Delete(payload);
		return -32603;
	}
	asap_envelope_clear(out);
	asap_envelope_init(out);
	out->id = strdup(ulid_buf);
	out->asap_version = strdup(ASAP_PROTOCOL_VERSION);
	out->sender = in->recipient ? strdup(in->recipient) : NULL;
	out->recipient = in->sender ? strdup(in->sender) : NULL;
	out->payload_type = strdup(payload_type);
	/* Ownership of payload moves to out; asap_envelope_clear frees it once. */
	out->payload = payload;
	if (in->correlation_id)
		out->correlation_id = strdup(in->correlation_id);
	if (in->trace_id)
		out->trace_id = strdup(in->trace_id);
	if (!out->id || !out->asap_version || !out->sender || !out->recipient || !out->payload_type) {
		asap_envelope_clear(out);
		asap_envelope_init(out);
		return -32603;
	}
	return 0;
}

const char *asap_resolve_task_session_id(const char *sender, char *buf, size_t buf_size)
{
	size_t need;
	int n;
	if (!sender || sender[0] == '\0')
		return "asap:inbound";
	if (!buf || buf_size == 0)
		return NULL;
	need = strlen("asap:") + strlen(sender) + 1;
	if (need > buf_size)
		return NULL;
	n = snprintf(buf, buf_size, "asap:%s", sender);
	if (n < 0 || (size_t)n >= buf_size)
		return NULL;
	return buf;
}

static int handle_task_request(const asap_envelope_t *in, asap_envelope_t *out,
	asap_server_ctx_t *ctx, char *err_message, size_t err_message_size)
{
	char *prompt;
	char *resp_buf;
	char sid_buf[ASAP_TASK_SESSION_ID_CAP];
	const char *sid;
	int ar;
	cJSON *pl;
	int fill_rc;
	prompt = task_payload_to_prompt(in->payload);
	if (!prompt || prompt[0] == '\0') {
		free(prompt);
		set_err(err_message, err_message_size, "task.request: missing input text in payload");
		return -32602;
	}
	resp_buf = (char *)malloc((size_t)ASAP_SERVER_AGENT_RESPONSE_CAP);
	if (!resp_buf) {
		free(prompt);
		set_err(err_message, err_message_size, "task.request: out of memory");
		return -32603;
	}
	resp_buf[0] = '\0';
	/* Isolate SQLite history by sender URN; "asap:inbound" mixed clients (#64).
	 * Ignore ctx->session_id so a later constant (including asap:inbound)
	 * cannot restore a shared bucket. Authenticity is sender_is_trusted():
	 * POST /asap is protocol-public; empty trusted_senders allows every URN. */
	sid = asap_resolve_task_session_id(in->sender, sid_buf, sizeof sid_buf);
	if (!sid) {
		char too_long[128];
		free(resp_buf);
		free(prompt);
		snprintf(too_long, sizeof too_long,
			"task.request: sender URN length %zu exceeds session id cap %d",
			in->sender ? strlen(in->sender) : 0, ASAP_TASK_SESSION_ID_CAP);
		set_err(err_message, err_message_size, too_long);
		return -32602;
	}
	if (ctx->task_request_hook)
		ar = ctx->task_request_hook(ctx, in, resp_buf, (size_t)ASAP_SERVER_AGENT_RESPONSE_CAP);
	else {
		if (!ctx->cfg || !ctx->provider) {
			free(resp_buf);
			free(prompt);
			set_err(err_message, err_message_size, "task.request: server missing cfg or provider");
			return -32603;
		}
		/* Acquire the global agent mutex: prevents concurrent session/memory
		 * access from multiple HTTP/WS threads (PRD §7, CR-5, option a). */
		agent_lock();
		ar = agent_run(ctx->cfg, sid, prompt, ctx->provider, ctx->tools, ctx->tool_count,
				resp_buf, (size_t)ASAP_SERVER_AGENT_RESPONSE_CAP);
		agent_unlock();
	}
	free(prompt);
	if (ar != 0) {
		set_err(err_message, err_message_size, resp_buf[0] ? resp_buf : "agent_run failed");
		free(resp_buf);
		return -32603;
	}
	pl = cJSON_CreateObject();
	if (!pl || !cJSON_AddStringToObject(pl, "output", resp_buf)) {
		free(resp_buf);
		if (pl) cJSON_Delete(pl);
		set_err(err_message, err_message_size, "task.request: failed to build response payload");
		return -32603;
	}
	free(resp_buf);
	fill_rc = fill_response_envelope(out, in, "task.response", pl);
	if (fill_rc != 0)
		set_err(err_message, err_message_size, "task.request: failed to build envelope");
	return fill_rc;
}

static int handle_task_cancel(const asap_envelope_t *in, asap_envelope_t *out,
	char *err_message, size_t err_message_size)
{
	cJSON *pl = cJSON_CreateObject();
	int rc;
	if (!pl || !cJSON_AddBoolToObject(pl, "cancelled", 0) ||
		!cJSON_AddStringToObject(pl, "reason", "cancellation not supported in this build")) {
		if (pl) cJSON_Delete(pl);
		set_err(err_message, err_message_size, "task.cancel: allocation failed");
		return -32603;
	}
	rc = fill_response_envelope(out, in, "task.response", pl);
	if (rc != 0)
		set_err(err_message, err_message_size, "task.cancel: failed to build envelope");
	return rc;
}

static int handle_state_query(const asap_envelope_t *in, asap_envelope_t *out,
	asap_server_ctx_t *ctx, char *err_message, size_t err_message_size)
{
	cJSON *pl = NULL;
	int rc;
	int sess = 0;
	int mem = 0;
	int cron = 0;
	int hook_rc = 0;
	int counts_rc = 0;
	/* Same mutex as mcp.tool_call / task.request: g_db is not safe from an
	 * HTTP thread while another thread is in agent_run (#60). */
	agent_lock();
	if (ctx->state_query_hook) {
		hook_rc = ctx->state_query_hook(ctx, &pl);
	} else {
		counts_rc = memory_get_row_counts(&sess, &mem, &cron);
	}
	agent_unlock();
	if (ctx->state_query_hook) {
		if (hook_rc != 0 || !pl) {
			set_err(err_message, err_message_size, "state.query: hook failed");
			return -32603;
		}
	} else {
		if (counts_rc != 0) {
			set_err(err_message, err_message_size, "state.query: memory store unavailable");
			return -32603;
		}
		pl = cJSON_CreateObject();
		if (!pl || !cJSON_AddNumberToObject(pl, "sessions", (double)sess) ||
			!cJSON_AddNumberToObject(pl, "memories", (double)mem) ||
			!cJSON_AddNumberToObject(pl, "cron_jobs", (double)cron)) {
			if (pl) cJSON_Delete(pl);
			set_err(err_message, err_message_size, "state.query: allocation failed");
			return -32603;
		}
	}
	rc = fill_response_envelope(out, in, "task.response", pl);
	if (rc != 0)
		set_err(err_message, err_message_size, "state.query: failed to build envelope");
	return rc;
}

static int args_json_from_payload(const cJSON *payload, char **args_json_out,
	char *err_message, size_t err_message_size)
{
	const cJSON *args_j = cJSON_GetObjectItemCaseSensitive(payload, "arguments");
	char *aj;
	if (!args_j) {
		aj = strdup("{}");
		if (!aj) {
			set_err(err_message, err_message_size, "mcp.tool_call: allocation failed");
			return -1;
		}
		*args_json_out = aj;
		return 0;
	}
	if (cJSON_IsObject((cJSON *)args_j) || cJSON_IsArray((cJSON *)args_j)) {
		aj = cJSON_PrintUnformatted((cJSON *)args_j);
		if (!aj) {
			set_err(err_message, err_message_size, "mcp.tool_call: allocation failed");
			return -1;
		}
		*args_json_out = aj;
		return 0;
	}
	if (cJSON_IsString(args_j) && args_j->valuestring) {
		aj = strdup(args_j->valuestring);
		if (!aj) {
			set_err(err_message, err_message_size, "mcp.tool_call: allocation failed");
			return -1;
		}
		*args_json_out = aj;
		return 0;
	}
	set_err(err_message, err_message_size, "mcp.tool_call: invalid arguments field");
	return -1;
}

static int handle_mcp_tool_call(const asap_envelope_t *in, asap_envelope_t *out,
	asap_server_ctx_t *ctx, char *err_message, size_t err_message_size)
{
	const cJSON *name_j = cJSON_GetObjectItemCaseSensitive(in->payload, "name");
	const char *tool_name;
	char *args_json = NULL;
	enum { RESULT_CAP = 65536 };
	char *result_buf;
	int exec_rc;
	cJSON *pl;
	int fill_rc;
	if (!name_j || !cJSON_IsString(name_j) || !name_j->valuestring) {
		set_err(err_message, err_message_size, "mcp.tool_call: missing name");
		return -32602;
	}
	tool_name = name_j->valuestring;
	if (args_json_from_payload(in->payload, &args_json, err_message, err_message_size) != 0)
		return -32602;
	result_buf = (char *)malloc(RESULT_CAP);
	if (!result_buf) {
		free(args_json);
		set_err(err_message, err_message_size, "mcp.tool_call: out of memory");
		return -32603;
	}
	result_buf[0] = '\0';
	/* Same mutex as task.request / handle_message: inbound tools may
	 * touch session/memory while another thread is in agent_run (#60). */
	agent_lock();
	if (ctx->tool_call_hook) {
		int hr = ctx->tool_call_hook(ctx, tool_name, args_json, result_buf, RESULT_CAP);
		exec_rc = hr == 0 ? 0 : 2;
	} else {
		exec_rc = dispatch_tool_by_name(ctx, tool_name, args_json, result_buf, RESULT_CAP);
	}
	agent_unlock();
	free(args_json);
	if (exec_rc == 1) {
		free(result_buf);
		set_err(err_message, err_message_size, "mcp.tool_call: unknown tool");
		return ASAP_SERVER_RPC_TOOL_NOT_FOUND;
	}
	if (exec_rc != 0) {
		set_err(err_message, err_message_size,
			result_buf[0] ? result_buf : "mcp.tool_call: tool execute failed");
		free(result_buf);
		return -32603;
	}
	pl = cJSON_CreateObject();
	if (!pl || !cJSON_AddStringToObject(pl, "result", result_buf)) {
		free(result_buf);
		if (pl) cJSON_Delete(pl);
		set_err(err_message, err_message_size, "mcp.tool_call: failed to build result payload");
		return -32603;
	}
	free(result_buf);
	fill_rc = fill_response_envelope(out, in, "mcp.tool_result", pl);
	if (fill_rc != 0)
		set_err(err_message, err_message_size, "mcp.tool_call: failed to build envelope");
	return fill_rc;
}

int asap_server_handle(const asap_envelope_t *in, asap_envelope_t *out,
	asap_server_ctx_t *ctx, char *err_message, size_t err_message_size)
{
	if (err_message && err_message_size > 0) err_message[0] = '\0';
	if (!in || !out || !ctx) {
		set_err(err_message, err_message_size, "invalid arguments");
		return -32602;
	}
	if (!in->payload_type || !in->payload) {
		set_err(err_message, err_message_size, "missing envelope fields");
		return -32602;
	}
	if (!sender_is_trusted(ctx->cfg, in->sender)) {
		fprintf(stderr, "asap: rejecting untrusted sender '%s'\n",
			in->sender ? in->sender : "(null)");
		set_err(err_message, err_message_size, "sender not trusted");
		return ASAP_SERVER_RPC_SENDER_UNTRUSTED;
	}
	asap_envelope_init(out);
	if (strcmp(in->payload_type, "task.response") == 0 ||
	    strcmp(in->payload_type, "mcp.tool_result") == 0) {
		set_err(err_message, err_message_size, "payload_type not accepted on inbound server");
		return -32601;
	}
	if (strcmp(in->payload_type, "task.request") == 0)
		return handle_task_request(in, out, ctx, err_message, err_message_size);
	if (strcmp(in->payload_type, "task.cancel") == 0)
		return handle_task_cancel(in, out, err_message, err_message_size);
	if (strcmp(in->payload_type, "state.query") == 0)
		return handle_state_query(in, out, ctx, err_message, err_message_size);
	if (strcmp(in->payload_type, "mcp.tool_call") == 0)
		return handle_mcp_tool_call(in, out, ctx, err_message, err_message_size);
	set_err(err_message, err_message_size, "unsupported payload_type");
	return -32601;
}
