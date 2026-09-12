/**
 * @file test_asap_server.c
 * @brief Unit tests for asap_server_handle (hooks + memory-backed state.query).
 */
#define _POSIX_C_SOURCE 200809L

#include "asap/server.h"
#include "asap/envelope.h"
#include "core/config.h"
#include "core/memory.h"
#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define ASSERT(c) do { if (!(c)) { fprintf(stderr, "FAIL: %s:%d %s\n", __FILE__, __LINE__, #c); return 1; } } while (0)

static int test_hook_task(const asap_server_ctx_t *ctx, const asap_envelope_t *in,
	char *response_buf, size_t response_cap)
{
	(void)ctx;
	(void)in;
	snprintf(response_buf, response_cap, "reply-for-test");
	return 0;
}

static int test_hook_state(const asap_server_ctx_t *ctx, cJSON **payload_out)
{
	cJSON *o;
	(void)ctx;
	o = cJSON_CreateObject();
	if (!o) return -1;
	if (!cJSON_AddNumberToObject(o, "custom", 42.0)) {
		cJSON_Delete(o);
		return -1;
	}
	*payload_out = o;
	return 0;
}

static int test_hook_state_fail(const asap_server_ctx_t *ctx, cJSON **payload_out)
{
	(void)ctx;
	(void)payload_out;
	return -1;
}

static int g_agent_mutex_held_during_mcp_tool;

static int echo_tool_execute(const char *args_json, char *result_buf, size_t max_len)
{
	(void)args_json;
	snprintf(result_buf, max_len, "echo-ok");
	return 0;
}

static int mutex_probe_tool_execute(const char *args_json, char *result_buf, size_t max_len)
{
	(void)args_json;
	g_agent_mutex_held_during_mcp_tool = agent_mutex_is_locked_for_test();
	snprintf(result_buf, max_len, "probe-ok");
	return 0;
}

static int failing_tool_execute(const char *args_json, char *result_buf, size_t max_len)
{
	(void)args_json;
	snprintf(result_buf, max_len, "tool-failure-payload");
	return -1;
}

static int hook_tool_dispatcher(const asap_server_ctx_t *ctx, const char *tool_name,
				const char *args_json, char *result_buf, size_t result_cap)
{
	(void)ctx;
	(void)args_json;
	if (tool_name && strcmp(tool_name, "echo") == 0) {
		snprintf(result_buf, result_cap, "hook-tool-ok");
		return 0;
	}
	snprintf(result_buf, result_cap, "hook-bad-tool");
	return -1;
}

static agent_tool_t s_echo_tool = {
	.name = "echo",
	.description = "",
	.parameters_json = "{}",
	.execute = echo_tool_execute,
};

static agent_tool_t s_mutex_probe_tool = {
	.name = "mutex_probe",
	.description = "",
	.parameters_json = "{}",
	.execute = mutex_probe_tool_execute,
};

static agent_tool_t s_flaky_tool = {
	.name = "flaky",
	.description = "",
	.parameters_json = "{}",
	.execute = failing_tool_execute,
};

static int build_in_custom(asap_envelope_t *e, const char *ptype, const char *sender,
	const char *recipient, cJSON *payload)
{
	cJSON *root;
	cJSON *dup_payload;
	int rc;
	asap_envelope_init(e);
	root = cJSON_CreateObject();
	if (!root) return -1;
	dup_payload = cJSON_Duplicate(payload, 1);
	if (!dup_payload) {
		cJSON_Delete(root);
		return -1;
	}
	if (!cJSON_AddStringToObject(root, "id", "e1") ||
	    !cJSON_AddStringToObject(root, "asap_version", "2.1") ||
	    !cJSON_AddStringToObject(root, "sender", sender) ||
	    !cJSON_AddStringToObject(root, "recipient", recipient) ||
	    !cJSON_AddStringToObject(root, "payload_type", ptype) ||
	    !cJSON_AddItemToObject(root, "payload", dup_payload)) {
		cJSON_Delete(root);
		return -1;
	}
	rc = asap_envelope_from_object(root, NULL, e, NULL);
	cJSON_Delete(root);
	return rc;
}

static int build_in(asap_envelope_t *e, const char *ptype, cJSON *payload)
{
	return build_in_custom(e, ptype, "urn:from", "urn:to", payload);
}

static int wrap_build(asap_envelope_t *e, const char *ptype, cJSON *payload)
{
	int rc = build_in(e, ptype, payload);
	cJSON_Delete(payload);
	return rc;
}

static void teardown_env(asap_envelope_t *e)
{
	asap_envelope_clear(e);
	asap_envelope_init(e);
}

static int test_task_request_hook(void)
{
	asap_envelope_t in;
	asap_envelope_t out;
	asap_server_ctx_t ctx;
	char err[128];
	cJSON *pl;
	int rc;
	pl = cJSON_CreateObject();
	ASSERT(pl != NULL);
	ASSERT(cJSON_AddStringToObject(pl, "input", "hi") != NULL);
	ASSERT(wrap_build(&in, "task.request", pl) == 0);
	memset(&ctx, 0, sizeof ctx);
	ctx.task_request_hook = test_hook_task;
	rc = asap_server_handle(&in, &out, &ctx, err, sizeof err);
	ASSERT(rc == 0);
	ASSERT(out.payload != NULL);
	ASSERT(strcmp(out.payload_type, "task.response") == 0);
	ASSERT(strcmp(out.sender, "urn:to") == 0);
	ASSERT(strcmp(out.recipient, "urn:from") == 0);
	{
		const cJSON *o = cJSON_GetObjectItemCaseSensitive(out.payload, "output");
		ASSERT(o && cJSON_IsString(o) && strcmp(o->valuestring, "reply-for-test") == 0);
	}
	teardown_env(&in);
	teardown_env(&out);
	return 0;
}

static int test_task_cancel_stub(void)
{
	asap_envelope_t in;
	asap_envelope_t out;
	asap_server_ctx_t ctx;
	char err[128];
	cJSON *pl;
	int rc;
	pl = cJSON_CreateObject();
	ASSERT(pl != NULL);
	ASSERT(cJSON_AddNullToObject(pl, "task_id") != NULL);
	ASSERT(wrap_build(&in, "task.cancel", pl) == 0);
	memset(&ctx, 0, sizeof ctx);
	rc = asap_server_handle(&in, &out, &ctx, err, sizeof err);
	ASSERT(rc == 0);
	ASSERT(strcmp(out.payload_type, "task.response") == 0);
	{
		const cJSON *c = cJSON_GetObjectItemCaseSensitive(out.payload, "cancelled");
		ASSERT(c && cJSON_IsFalse(c));
	}
	teardown_env(&in);
	teardown_env(&out);
	return 0;
}

static int test_state_query_hook(void)
{
	asap_envelope_t in;
	asap_envelope_t out;
	asap_server_ctx_t ctx;
	char err[128];
	cJSON *pl;
	int rc;
	pl = cJSON_CreateObject();
	ASSERT(pl != NULL);
	ASSERT(cJSON_AddBoolToObject(pl, "full", 1) != NULL);
	ASSERT(wrap_build(&in, "state.query", pl) == 0);
	memset(&ctx, 0, sizeof ctx);
	ctx.state_query_hook = test_hook_state;
	rc = asap_server_handle(&in, &out, &ctx, err, sizeof err);
	ASSERT(rc == 0);
	{
		const cJSON *c = cJSON_GetObjectItemCaseSensitive(out.payload, "custom");
		ASSERT(c && cJSON_IsNumber(c) && c->valuedouble == 42.0);
	}
	teardown_env(&in);
	teardown_env(&out);
	return 0;
}

static int test_state_query_memory_counts(void)
{
	char tmpl[] = "/tmp/sc_asap_srv_XXXXXX";
	int fd;
	asap_envelope_t in;
	asap_envelope_t out;
	asap_server_ctx_t ctx;
	char err[128];
	cJSON *pl;
	int rc;
	fd = mkstemp(tmpl);
	ASSERT(fd >= 0);
	close(fd);
	ASSERT(memory_init(tmpl) == 0);
	pl = cJSON_CreateObject();
	ASSERT(pl != NULL);
	ASSERT(wrap_build(&in, "state.query", pl) == 0);
	memset(&ctx, 0, sizeof ctx);
	rc = asap_server_handle(&in, &out, &ctx, err, sizeof err);
	ASSERT(rc == 0);
	{
		const cJSON *s = cJSON_GetObjectItemCaseSensitive(out.payload, "sessions");
		const cJSON *m = cJSON_GetObjectItemCaseSensitive(out.payload, "memories");
		const cJSON *c = cJSON_GetObjectItemCaseSensitive(out.payload, "cron_jobs");
		ASSERT(s && cJSON_IsNumber(s) && s->valuedouble == 0.0);
		ASSERT(m && cJSON_IsNumber(m) && m->valuedouble == 0.0);
		ASSERT(c && cJSON_IsNumber(c) && c->valuedouble == 0.0);
	}
	memory_cleanup();
	unlink(tmpl);
	teardown_env(&in);
	teardown_env(&out);
	return 0;
}

static int test_mcp_tool_echo(void)
{
	asap_envelope_t in;
	asap_envelope_t out;
	asap_server_ctx_t ctx;
	char err[128];
	cJSON *pl;
	cJSON *args;
	int rc;
	pl = cJSON_CreateObject();
	args = cJSON_CreateObject();
	ASSERT(pl != NULL && args != NULL);
	ASSERT(cJSON_AddStringToObject(pl, "name", "echo") != NULL);
	ASSERT(cJSON_AddItemToObject(pl, "arguments", args));
	ASSERT(wrap_build(&in, "mcp.tool_call", pl) == 0);
	memset(&ctx, 0, sizeof ctx);
	ctx.tools = &s_echo_tool;
	ctx.tool_count = 1;
	rc = asap_server_handle(&in, &out, &ctx, err, sizeof err);
	ASSERT(rc == 0);
	ASSERT(strcmp(out.payload_type, "mcp.tool_result") == 0);
	{
		const cJSON *r = cJSON_GetObjectItemCaseSensitive(out.payload, "result");
		ASSERT(r && cJSON_IsString(r) && strcmp(r->valuestring, "echo-ok") == 0);
	}
	teardown_env(&in);
	teardown_env(&out);
	return 0;
}

static int test_mcp_unknown_tool(void)
{
	asap_envelope_t in;
	asap_envelope_t out;
	asap_server_ctx_t ctx;
	char err[128];
	cJSON *pl;
	int rc;
	pl = cJSON_CreateObject();
	ASSERT(pl != NULL);
	ASSERT(cJSON_AddStringToObject(pl, "name", "missing_tool") != NULL);
	ASSERT(cJSON_AddObjectToObject(pl, "arguments") != NULL);
	ASSERT(wrap_build(&in, "mcp.tool_call", pl) == 0);
	memset(&ctx, 0, sizeof ctx);
	ctx.tools = &s_echo_tool;
	ctx.tool_count = 1;
	rc = asap_server_handle(&in, &out, &ctx, err, sizeof err);
	ASSERT(rc == ASAP_SERVER_RPC_TOOL_NOT_FOUND);
	teardown_env(&in);
	teardown_env(&out);
	return 0;
}

static int test_reject_task_response_inbound(void)
{
	asap_envelope_t in;
	asap_envelope_t out;
	asap_server_ctx_t ctx;
	char err[128];
	cJSON *pl;
	int rc;
	pl = cJSON_CreateObject();
	ASSERT(pl != NULL);
	ASSERT(wrap_build(&in, "task.response", pl) == 0);
	memset(&ctx, 0, sizeof ctx);
	rc = asap_server_handle(&in, &out, &ctx, err, sizeof err);
	ASSERT(rc == -32601);
	teardown_env(&in);
	teardown_env(&out);
	return 0;
}

static int test_task_request_missing_input(void)
{
	asap_envelope_t in;
	asap_envelope_t out;
	asap_server_ctx_t ctx;
	char err[128];
	cJSON *pl;
	int rc;
	pl = cJSON_CreateObject();
	ASSERT(pl != NULL);
	ASSERT(cJSON_AddStringToObject(pl, "input", "") != NULL);
	ASSERT(wrap_build(&in, "task.request", pl) == 0);
	memset(&ctx, 0, sizeof ctx);
	ctx.task_request_hook = test_hook_task;
	rc = asap_server_handle(&in, &out, &ctx, err, sizeof err);
	ASSERT(rc == -32602);
	teardown_env(&in);
	teardown_env(&out);
	return 0;
}

static int test_task_request_no_agent_config(void)
{
	asap_envelope_t in;
	asap_envelope_t out;
	asap_server_ctx_t ctx;
	char err[128];
	cJSON *pl;
	int rc;
	pl = cJSON_CreateObject();
	ASSERT(pl != NULL);
	ASSERT(cJSON_AddStringToObject(pl, "input", "x") != NULL);
	ASSERT(wrap_build(&in, "task.request", pl) == 0);
	memset(&ctx, 0, sizeof ctx);
	rc = asap_server_handle(&in, &out, &ctx, err, sizeof err);
	ASSERT(rc == -32603);
	teardown_env(&in);
	teardown_env(&out);
	return 0;
}

static int test_trust_list_allows_listed_sender(void)
{
	const char *path = "/tmp/shellclaw_test_asap_trust_ok.toml";
	FILE *f;
	config_t *cfg;
	asap_envelope_t in;
	asap_envelope_t out;
	asap_server_ctx_t ctx;
	char err[128];
	cJSON *pl;
	int rc;
	f = fopen(path, "w");
	ASSERT(f != NULL);
	fprintf(f, "[agent]\nmodel = \"test\"\n");
	fprintf(f, "[asap]\ntrusted_senders = [\"urn:from\"]\n");
	fclose(f);
	ASSERT(config_load(path, &cfg, NULL, 0) == 0);
	pl = cJSON_CreateObject();
	ASSERT(pl != NULL);
	ASSERT(cJSON_AddStringToObject(pl, "input", "hi") != NULL);
	ASSERT(wrap_build(&in, "task.request", pl) == 0);
	memset(&ctx, 0, sizeof ctx);
	ctx.cfg = cfg;
	ctx.task_request_hook = test_hook_task;
	rc = asap_server_handle(&in, &out, &ctx, err, sizeof err);
	ASSERT(rc == 0);
	config_free(cfg);
	remove(path);
	teardown_env(&in);
	teardown_env(&out);
	return 0;
}

static int test_trust_list_rejects_unknown_sender(void)
{
	const char *path = "/tmp/shellclaw_test_asap_trust_deny.toml";
	FILE *f;
	config_t *cfg;
	asap_envelope_t in;
	asap_envelope_t out;
	asap_server_ctx_t ctx;
	char err[128];
	cJSON *pl;
	int rc;
	f = fopen(path, "w");
	ASSERT(f != NULL);
	fprintf(f, "[agent]\nmodel = \"test\"\n");
	fprintf(f, "[asap]\ntrusted_senders = [\"urn:other-only\"]\n");
	fclose(f);
	ASSERT(config_load(path, &cfg, NULL, 0) == 0);
	pl = cJSON_CreateObject();
	ASSERT(pl != NULL);
	ASSERT(cJSON_AddStringToObject(pl, "input", "hi") != NULL);
	ASSERT(wrap_build(&in, "task.request", pl) == 0);
	memset(&ctx, 0, sizeof ctx);
	ctx.cfg = cfg;
	ctx.task_request_hook = test_hook_task;
	rc = asap_server_handle(&in, &out, &ctx, err, sizeof err);
	ASSERT(rc == ASAP_SERVER_RPC_SENDER_UNTRUSTED);
	config_free(cfg);
	remove(path);
	teardown_env(&in);
	teardown_env(&out);
	return 0;
}

static int test_null_envelope_arguments(void)
{
	asap_envelope_t out;
	asap_server_ctx_t ctx;
	char err[96];
	memset(&ctx, 0, sizeof ctx);
	ASSERT(asap_server_handle(NULL, &out, &ctx, err, sizeof err) == -32602);
	return 0;
}

static int test_state_query_hook_failure(void)
{
	asap_envelope_t in;
	asap_envelope_t out;
	asap_server_ctx_t ctx;
	char err[128];
	cJSON *pl;
	int rc;
	pl = cJSON_CreateObject();
	ASSERT(pl != NULL);
	ASSERT(cJSON_AddBoolToObject(pl, "full", 1) != NULL);
	ASSERT(wrap_build(&in, "state.query", pl) == 0);
	memset(&ctx, 0, sizeof ctx);
	ctx.state_query_hook = test_hook_state_fail;
	rc = asap_server_handle(&in, &out, &ctx, err, sizeof err);
	ASSERT(rc == -32603);
	ASSERT(strstr(err, "state.query") != NULL);
	teardown_env(&in);
	teardown_env(&out);
	return 0;
}

static int test_state_query_without_memory(void)
{
	asap_envelope_t in;
	asap_envelope_t out;
	asap_server_ctx_t ctx;
	char err[128];
	cJSON *pl;
	int rc;
	memory_cleanup();
	pl = cJSON_CreateObject();
	ASSERT(pl != NULL);
	ASSERT(wrap_build(&in, "state.query", pl) == 0);
	memset(&ctx, 0, sizeof ctx);
	rc = asap_server_handle(&in, &out, &ctx, err, sizeof err);
	ASSERT(rc == -32603);
	ASSERT(strstr(err, "memory") != NULL || strstr(err, "unavailable") != NULL);
	teardown_env(&in);
	teardown_env(&out);
	return 0;
}

static int test_task_request_accepts_prompt_field(void)
{
	asap_envelope_t in;
	asap_envelope_t out;
	asap_server_ctx_t ctx;
	char err[128];
	cJSON *pl;
	int rc;
	pl = cJSON_CreateObject();
	ASSERT(pl != NULL);
	ASSERT(cJSON_AddStringToObject(pl, "prompt", "from-prompt") != NULL);
	ASSERT(wrap_build(&in, "task.request", pl) == 0);
	memset(&ctx, 0, sizeof ctx);
	ctx.task_request_hook = test_hook_task;
	rc = asap_server_handle(&in, &out, &ctx, err, sizeof err);
	ASSERT(rc == 0);
	ASSERT(strcmp(out.payload_type, "task.response") == 0);
	teardown_env(&in);
	teardown_env(&out);
	return 0;
}

static int test_task_request_accepts_message_field(void)
{
	asap_envelope_t in;
	asap_envelope_t out;
	asap_server_ctx_t ctx;
	char err[128];
	cJSON *pl;
	int rc;
	pl = cJSON_CreateObject();
	ASSERT(pl != NULL);
	ASSERT(cJSON_AddStringToObject(pl, "message", "from-message") != NULL);
	ASSERT(wrap_build(&in, "task.request", pl) == 0);
	memset(&ctx, 0, sizeof ctx);
	ctx.task_request_hook = test_hook_task;
	rc = asap_server_handle(&in, &out, &ctx, err, sizeof err);
	ASSERT(rc == 0);
	ASSERT(strcmp(out.payload_type, "task.response") == 0);
	teardown_env(&in);
	teardown_env(&out);
	return 0;
}

static int test_task_request_object_payload_fallback(void)
{
	asap_envelope_t in;
	asap_envelope_t out;
	asap_server_ctx_t ctx;
	char err[128];
	cJSON *pl;
	int rc;
	pl = cJSON_CreateObject();
	ASSERT(pl != NULL);
	ASSERT(cJSON_AddBoolToObject(pl, "only_flag", 0) != NULL);
	ASSERT(wrap_build(&in, "task.request", pl) == 0);
	memset(&ctx, 0, sizeof ctx);
	ctx.task_request_hook = test_hook_task;
	rc = asap_server_handle(&in, &out, &ctx, err, sizeof err);
	ASSERT(rc == 0);
	ASSERT(strcmp(out.payload_type, "task.response") == 0);
	teardown_env(&in);
	teardown_env(&out);
	return 0;
}

static int test_unsupported_payload_type(void)
{
	asap_envelope_t in;
	asap_envelope_t out;
	asap_server_ctx_t ctx;
	char err[160];
	int rc;

	asap_envelope_init(&in);
	in.id = strdup("unsupported-e1");
	in.asap_version = strdup("2.1");
	in.sender = strdup("urn:from");
	in.recipient = strdup("urn:to");
	in.payload_type = strdup("telemetry.event");
	ASSERT(in.id && in.asap_version && in.sender && in.recipient && in.payload_type);
	in.payload = cJSON_CreateObject();
	ASSERT(in.payload != NULL);
	ASSERT(cJSON_AddNumberToObject(in.payload, "x", 1.0) != NULL);
	memset(&ctx, 0, sizeof ctx);
	rc = asap_server_handle(&in, &out, &ctx, err, sizeof err);
	ASSERT(rc == -32601);
	teardown_env(&in);
	teardown_env(&out);
	return 0;
}

static int test_reject_mcp_tool_result_inbound(void)
{
	asap_envelope_t in;
	asap_envelope_t out;
	asap_server_ctx_t ctx;
	char err[128];
	cJSON *pl;
	int rc;
	pl = cJSON_CreateObject();
	ASSERT(pl != NULL);
	ASSERT(wrap_build(&in, "mcp.tool_result", pl) == 0);
	memset(&ctx, 0, sizeof ctx);
	rc = asap_server_handle(&in, &out, &ctx, err, sizeof err);
	ASSERT(rc == -32601);
	teardown_env(&in);
	teardown_env(&out);
	return 0;
}

static int test_mcp_arguments_string_encoded_json(void)
{
	asap_envelope_t in;
	asap_envelope_t out;
	asap_server_ctx_t ctx;
	char err[160];
	cJSON *pl;
	int rc;
	pl = cJSON_CreateObject();
	ASSERT(pl != NULL);
	ASSERT(cJSON_AddStringToObject(pl, "name", "echo") != NULL);
	ASSERT(cJSON_AddStringToObject(pl, "arguments", "{\"k\":\"v\"}") != NULL);
	ASSERT(wrap_build(&in, "mcp.tool_call", pl) == 0);
	memset(&ctx, 0, sizeof ctx);
	ctx.tools = &s_echo_tool;
	ctx.tool_count = 1;
	rc = asap_server_handle(&in, &out, &ctx, err, sizeof err);
	ASSERT(rc == 0);
	ASSERT(strcmp(out.payload_type, "mcp.tool_result") == 0);
	teardown_env(&in);
	teardown_env(&out);
	return 0;
}

static int test_mcp_omitted_arguments_defaults_to_empty_object(void)
{
	asap_envelope_t in;
	asap_envelope_t out;
	asap_server_ctx_t ctx;
	char err[128];
	cJSON *pl;
	int rc;
	pl = cJSON_CreateObject();
	ASSERT(pl != NULL);
	ASSERT(cJSON_AddStringToObject(pl, "name", "echo") != NULL);
	ASSERT(wrap_build(&in, "mcp.tool_call", pl) == 0);
	memset(&ctx, 0, sizeof ctx);
	ctx.tools = &s_echo_tool;
	ctx.tool_count = 1;
	rc = asap_server_handle(&in, &out, &ctx, err, sizeof err);
	ASSERT(rc == 0);
	teardown_env(&in);
	teardown_env(&out);
	return 0;
}

static int test_tool_call_hook_overrides_builtin_dispatch(void)
{
	asap_envelope_t in;
	asap_envelope_t out;
	asap_server_ctx_t ctx;
	char err[128];
	cJSON *pl;
	int rc;
	pl = cJSON_CreateObject();
	ASSERT(pl != NULL);
	ASSERT(cJSON_AddStringToObject(pl, "name", "echo") != NULL);
	ASSERT(cJSON_AddObjectToObject(pl, "arguments") != NULL);
	ASSERT(wrap_build(&in, "mcp.tool_call", pl) == 0);
	memset(&ctx, 0, sizeof ctx);
	ctx.tool_call_hook = hook_tool_dispatcher;
	rc = asap_server_handle(&in, &out, &ctx, err, sizeof err);
	ASSERT(rc == 0);
	ASSERT(strcmp(out.payload_type, "mcp.tool_result") == 0);
	{
		const cJSON *r = cJSON_GetObjectItemCaseSensitive(out.payload, "result");
		ASSERT(r && cJSON_IsString(r) && strcmp(r->valuestring, "hook-tool-ok") == 0);
	}
	teardown_env(&in);
	teardown_env(&out);
	return 0;
}

static int test_tool_execute_nonzero_reports_error(void)
{
	asap_envelope_t in;
	asap_envelope_t out;
	asap_server_ctx_t ctx;
	char err[160];
	cJSON *pl;
	int rc;
	pl = cJSON_CreateObject();
	ASSERT(pl != NULL);
	ASSERT(cJSON_AddStringToObject(pl, "name", "flaky") != NULL);
	ASSERT(wrap_build(&in, "mcp.tool_call", pl) == 0);
	memset(&ctx, 0, sizeof ctx);
	ctx.tools = &s_flaky_tool;
	ctx.tool_count = 1;
	rc = asap_server_handle(&in, &out, &ctx, err, sizeof err);
	ASSERT(rc == -32603);
	ASSERT(strstr(err, "failure") != NULL || strstr(err, "tool") != NULL);
	teardown_env(&in);
	teardown_env(&out);
	return 0;
}

static int test_trust_sender_rejects_blank_sender_when_list_nonempty(void)
{
	const char *path = "/tmp/shellclaw_test_asap_trust_blank.toml";
	FILE *f;
	config_t *cfg;
	asap_envelope_t in;
	asap_envelope_t out;
	asap_server_ctx_t ctx;
	char err[160];
	cJSON *pl;
	int rc;
	f = fopen(path, "w");
	ASSERT(f != NULL);
	fprintf(f, "[agent]\nmodel = \"test\"\n");
	fprintf(f, "[asap]\ntrusted_senders = [\"urn:someone\"]\n");
	fclose(f);
	ASSERT(config_load(path, &cfg, NULL, 0) == 0);
	pl = cJSON_CreateObject();
	ASSERT(pl != NULL);
	ASSERT(cJSON_AddStringToObject(pl, "input", "hi") != NULL);
	ASSERT(build_in_custom(&in, "task.request", "", "urn:to", pl) == 0);
	cJSON_Delete(pl);
	memset(&ctx, 0, sizeof ctx);
	ctx.cfg = cfg;
	ctx.task_request_hook = test_hook_task;
	rc = asap_server_handle(&in, &out, &ctx, err, sizeof err);
	ASSERT(rc == ASAP_SERVER_RPC_SENDER_UNTRUSTED);
	config_free(cfg);
	remove(path);
	teardown_env(&in);
	teardown_env(&out);
	return 0;
}

static int test_mcp_tool_call_holds_agent_mutex(void)
{
	asap_envelope_t in;
	asap_envelope_t out;
	asap_server_ctx_t ctx;
	char err[128];
	cJSON *pl;
	int rc;
	asap_envelope_init(&in);
	asap_envelope_init(&out);
	pl = cJSON_CreateObject();
	ASSERT(pl != NULL);
	ASSERT(cJSON_AddStringToObject(pl, "name", "mutex_probe") != NULL);
	ASSERT(cJSON_AddObjectToObject(pl, "arguments") != NULL);
	ASSERT(wrap_build(&in, "mcp.tool_call", pl) == 0);
	memset(&ctx, 0, sizeof ctx);
	ctx.tools = &s_mutex_probe_tool;
	ctx.tool_count = 1;
	g_agent_mutex_held_during_mcp_tool = 0;
	rc = asap_server_handle(&in, &out, &ctx, err, sizeof err);
	ASSERT(rc == 0);
	ASSERT(g_agent_mutex_held_during_mcp_tool == 1);
	ASSERT(agent_mutex_is_locked_for_test() == 0);
	teardown_env(&in);
	teardown_env(&out);
	return 0;
}

int main(void)
{
	int r = 0;
	r |= test_task_request_hook();
	r |= test_task_cancel_stub();
	r |= test_state_query_hook();
	r |= test_state_query_memory_counts();
	r |= test_mcp_tool_echo();
	r |= test_mcp_unknown_tool();
	r |= test_reject_task_response_inbound();
	r |= test_task_request_missing_input();
	r |= test_task_request_no_agent_config();
	r |= test_trust_list_allows_listed_sender();
	r |= test_trust_list_rejects_unknown_sender();
	r |= test_null_envelope_arguments();
	r |= test_state_query_hook_failure();
	r |= test_state_query_without_memory();
	r |= test_task_request_accepts_prompt_field();
	r |= test_task_request_accepts_message_field();
	r |= test_task_request_object_payload_fallback();
	r |= test_unsupported_payload_type();
	r |= test_reject_mcp_tool_result_inbound();
	r |= test_mcp_arguments_string_encoded_json();
	r |= test_mcp_omitted_arguments_defaults_to_empty_object();
	r |= test_tool_call_hook_overrides_builtin_dispatch();
	r |= test_tool_execute_nonzero_reports_error();
	r |= test_trust_sender_rejects_blank_sender_when_list_nonempty();
	r |= test_mcp_tool_call_holds_agent_mutex();
	return r;
}
