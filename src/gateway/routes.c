/**
 * @file routes.c
 * @brief REST route handlers and dispatch for the gateway HTTP API.
 */
#define _POSIX_C_SOURCE 200809L

#include "gateway/routes.h"
#include "gateway/routes_hardware.h"
#include "gateway/auth.h"
#include "gateway/rate_limit.h"
#include "channels/channel.h"
#include "asap/manifest.h"
#include "asap/manifest_keys.h"
#include "asap/envelope.h"
#include "asap/server.h"
#include "asap/log.h"
#include "core/config.h"
#include "core/memory.h"
#include "core/skill.h"
#include "providers/provider.h"
#include "tools/context.h"
#include "tools/cron.h"
#include "cJSON.h"
#include <libwebsockets.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static void json_response(char *buf, size_t size, int *status, const char *json)
{
	if (!buf || size == 0 || !status) return;
	*status = 200;
	size_t len = strlen(json);
	if (len >= size) len = size - 1;
	memcpy(buf, json, len);
	buf[len] = '\0';
}

/** Print @p obj to @p buf via json_response; returns 0 on success, -1 on OOM/print failure. */
int json_print_to_buf(cJSON *obj, char *buf, size_t size, int *status)
{
	char *s;
	if (!obj) {
		json_error(buf, size, status, 500, "Internal error");
		return -1;
	}
	s = cJSON_PrintUnformatted(obj);
	if (!s) {
		json_error(buf, size, status, 500, "Internal error");
		return -1;
	}
	json_response(buf, size, status, s);
	free(s);
	return 0;
}

#define MEMORY_API_LIMIT_DEFAULT 20
#define MEMORY_API_LIMIT_MAX 100

static int parse_memory_limit_arg(const char *raw)
{
	char *end = NULL;
	long val;
	if (!raw || !raw[0]) return MEMORY_API_LIMIT_DEFAULT;
	val = strtol(raw, &end, 10);
	if (end == raw || (end && *end != '\0')) return MEMORY_API_LIMIT_DEFAULT;
	if (val < 1) return 1;
	if (val > MEMORY_API_LIMIT_MAX) return MEMORY_API_LIMIT_MAX;
	return (int)val;
}

void json_error(char *buf, size_t size, int *status, int code, const char *msg)
{
	if (!buf || size == 0 || !status) return;
	*status = code;
	cJSON *obj = cJSON_CreateObject();
	if (obj) {
		cJSON_AddItemToObject(obj, "error", cJSON_CreateString(msg));
		char *s = cJSON_PrintUnformatted(obj);
		cJSON_Delete(obj);
		if (s) {
			size_t len = strlen(s);
			if (len >= size) len = size - 1;
			memcpy(buf, s, len);
			buf[len] = '\0';
			free(s);
		}
	}
}

static void handle_health(http_server_ctx_t *ctx, char *buf, size_t size, int *status)
{
	time_t now = time(NULL);
	long uptime = (long)(now - ctx->start_time);
	cJSON *obj = cJSON_CreateObject();
	if (!obj) { json_error(buf, size, status, 500, "Internal error"); return; }
	cJSON_AddItemToObject(obj, "status", cJSON_CreateString("ok"));
	cJSON_AddItemToObject(obj, "uptime", cJSON_CreateNumber((double)uptime));
	cJSON_AddItemToObject(obj, "version", cJSON_CreateString(GATEWAY_VERSION));
	char *s = cJSON_PrintUnformatted(obj);
	cJSON_Delete(obj);
	if (s) {
		json_response(buf, size, status, s);
		free(s);
	} else {
		json_error(buf, size, status, 500, "Internal error");
	}
}

static void handle_pair(http_server_ctx_t *ctx, struct lws *wsi, const char *body, size_t body_len,
                        char *buf, size_t size, int *status)
{
	char code[16] = {0};
	if (!ctx || !ctx->auth) {
		json_error(buf, size, status, 500, "Internal error");
		return;
	}
	if (body_len > 0 && body) {
		cJSON *root = cJSON_ParseWithLength(body, body_len);
		if (root) {
			cJSON *c = cJSON_GetObjectItem(root, "code");
			if (cJSON_IsString(c) && c->valuestring) {
				strncpy(code, c->valuestring, sizeof(code) - 1);
			}
			c = cJSON_GetObjectItem(root, "pairing_code");
			if (!code[0] && cJSON_IsString(c) && c->valuestring)
				strncpy(code, c->valuestring, sizeof(code) - 1);
			cJSON_Delete(root);
		}
	}
	if (!code[0]) {
		int n = lws_hdr_custom_copy(wsi, code, sizeof(code), "X-Pairing-Code", 14);
		(void)n;
	}
	if (!code[0] && getenv("SHELLCLAW_TEST_MODE")) {
		const char *home = getenv("HOME");
		if (home && home[0] != '\0') {
			char path[512];
			FILE *tf;
			snprintf(path, sizeof(path), "%s/.shellclaw/test_pairing_code", home);
			tf = fopen(path, "r");
			if (tf) {
				if (fscanf(tf, "%6[0-9]", code) != 1)
					code[0] = '\0';
				fclose(tf);
			}
		}
	}
	char token[64] = {0};
	if (auth_pair(ctx->auth, code, token, sizeof(token)) != 0) {
		json_error(buf, size, status, 400, "Invalid pairing code");
		return;
	}
	cJSON *obj = cJSON_CreateObject();
	if (!obj) { json_error(buf, size, status, 500, "Internal error"); return; }
	cJSON_AddItemToObject(obj, "token", cJSON_CreateString(token));
	char *s = cJSON_PrintUnformatted(obj);
	cJSON_Delete(obj);
	if (s) {
		*status = 200;
		json_response(buf, size, status, s);
		free(s);
	} else {
		json_error(buf, size, status, 500, "Internal error");
	}
}

static void handle_config_get(const config_t *cfg, char *buf, size_t size, int *status)
{
	cJSON *obj = cJSON_CreateObject();
	if (!obj) { json_error(buf, size, status, 500, "Internal error"); return; }
	cJSON_AddItemToObject(obj, "model", cJSON_CreateString(config_agent_model(cfg) ?: ""));
	cJSON_AddItemToObject(obj, "max_tokens", cJSON_CreateNumber(config_agent_max_tokens(cfg)));
	cJSON_AddItemToObject(obj, "temperature", cJSON_CreateNumber(config_agent_temperature(cfg)));
	cJSON_AddItemToObject(obj, "gateway_host", cJSON_CreateString(config_gateway_host(cfg) ?: ""));
	cJSON_AddItemToObject(obj, "gateway_port", cJSON_CreateNumber(config_gateway_port(cfg)));
	char *s = cJSON_PrintUnformatted(obj);
	cJSON_Delete(obj);
	if (s) {
		json_response(buf, size, status, s);
		free(s);
	} else {
		json_error(buf, size, status, 500, "Internal error");
	}
}

static void handle_config_put(http_server_ctx_t *ctx, const char *body, size_t body_len,
                              char *buf, size_t size, int *status)
{
	if (!ctx->config_path || !body || body_len == 0) {
		json_error(buf, size, status, 400, "Bad request");
		return;
	}
	if (body_len > CONFIG_BODY_MAX) {
		json_error(buf, size, status, 400, "Config too large");
		return;
	}
	size_t path_len = strlen(ctx->config_path);
	char *tmp_path = malloc(path_len + 8);
	if (!tmp_path) { json_error(buf, size, status, 500, "Out of memory"); return; }
	snprintf(tmp_path, path_len + 8, "%s.tmp", ctx->config_path);
	FILE *f = fopen(tmp_path, "w");
	if (!f) {
		free(tmp_path);
		json_error(buf, size, status, 500, "Failed to write config");
		return;
	}
	size_t written = fwrite(body, 1, body_len, f);
	fclose(f);
	if (written != body_len) {
		unlink(tmp_path);
		free(tmp_path);
		json_error(buf, size, status, 500, "Failed to write config");
		return;
	}
	config_t *cfg = NULL;
	char errbuf[256] = {0};
	if (config_load(tmp_path, &cfg, errbuf, sizeof(errbuf)) != 0) {
		unlink(tmp_path);
		free(tmp_path);
		json_error(buf, size, status, 400, errbuf[0] ? errbuf : "Invalid TOML");
		return;
	}
	config_free(cfg);
	if (rename(tmp_path, ctx->config_path) != 0) {
		unlink(tmp_path);
		free(tmp_path);
		json_error(buf, size, status, 500, "Failed to save config");
		return;
	}
	free(tmp_path);
	*status = 200;
	json_response(buf, size, status, "{\"ok\":true}");
}

static void handle_skills_list(const config_t *cfg, char *buf, size_t size, int *status)
{
	char *names[64];
	int n = skill_list_names(cfg, names, 64);
	cJSON *arr = cJSON_CreateArray();
	if (!arr) { json_error(buf, size, status, 500, "Internal error"); return; }
	for (int i = 0; i < n; i++) {
		cJSON_AddItemToArray(arr, cJSON_CreateString(names[i]));
		free(names[i]);
	}
	char *s = cJSON_PrintUnformatted(arr);
	cJSON_Delete(arr);
	if (s) {
		json_response(buf, size, status, s);
		free(s);
	} else {
		json_error(buf, size, status, 500, "Internal error");
	}
}

#define SKILL_CONTENT_MAX 32768
#define MEMORY_RESULTS_MAX 16384

static void handle_skill_get(const config_t *cfg, const char *name, char *buf, size_t size, int *status)
{
	char *content = malloc(SKILL_CONTENT_MAX);
	if (!content) { json_error(buf, size, status, 500, "Out of memory"); return; }
	if (skill_get_content(cfg, name, content, SKILL_CONTENT_MAX) != 0) {
		free(content);
		json_error(buf, size, status, 404, "Skill not found");
		return;
	}
	cJSON *obj = cJSON_CreateObject();
	if (!obj) { free(content); json_error(buf, size, status, 500, "Internal error"); return; }
	cJSON_AddItemToObject(obj, "name", cJSON_CreateString(name));
	cJSON_AddItemToObject(obj, "content", cJSON_CreateString(content));
	free(content);
	char *s = cJSON_PrintUnformatted(obj);
	cJSON_Delete(obj);
	if (s) {
		json_response(buf, size, status, s);
		free(s);
	} else {
		json_error(buf, size, status, 500, "Internal error");
	}
}

static void handle_skill_create(const config_t *cfg, const char *body, size_t body_len,
                               char *buf, size_t size, int *status)
{
	cJSON *root = body ? cJSON_ParseWithLength(body, body_len) : NULL;
	if (!root) { json_error(buf, size, status, 400, "Invalid JSON"); return; }
	cJSON *name = cJSON_GetObjectItem(root, "name");
	cJSON *content = cJSON_GetObjectItem(root, "content");
	if (!cJSON_IsString(name) || !cJSON_IsString(content)) {
		cJSON_Delete(root);
		json_error(buf, size, status, 400, "name and content required");
		return;
	}
	int ret = skill_create(cfg, name->valuestring, content->valuestring);
	cJSON_Delete(root);
	if (ret != 0) {
		json_error(buf, size, status, 500, "Failed to create skill");
		return;
	}
	*status = 201;
	json_response(buf, size, status, "{\"ok\":true}");
}

static void handle_skill_update(const config_t *cfg, const char *name, const char *body, size_t body_len,
                                char *buf, size_t size, int *status)
{
	cJSON *root = body ? cJSON_ParseWithLength(body, body_len) : NULL;
	if (!root) { json_error(buf, size, status, 400, "Invalid JSON"); return; }
	cJSON *content = cJSON_GetObjectItem(root, "content");
	if (!cJSON_IsString(content)) {
		cJSON_Delete(root);
		json_error(buf, size, status, 400, "content required");
		return;
	}
	int ret = skill_update(cfg, name, content->valuestring);
	cJSON_Delete(root);
	if (ret != 0) {
		json_error(buf, size, status, 500, "Failed to update skill");
		return;
	}
	*status = 200;
	json_response(buf, size, status, "{\"ok\":true}");
}

static void handle_skill_delete(const config_t *cfg, const char *name,
                                char *buf, size_t size, int *status)
{
	if (skill_delete(cfg, name) != 0) {
		json_error(buf, size, status, 404, "Skill not found");
		return;
	}
	*status = 200;
	json_response(buf, size, status, "{\"ok\":true}");
}

static void handle_memory_get(const char *query, int limit, char *buf, size_t size, int *status)
{
	char *results = malloc(MEMORY_RESULTS_MAX);
	if (!results) { json_error(buf, size, status, 500, "Out of memory"); return; }
	if (memory_recall(query ? query : "", results, MEMORY_RESULTS_MAX, limit > 0 ? limit : 20) != 0) {
		free(results);
		json_error(buf, size, status, 500, "Memory recall failed");
		return;
	}
	cJSON *obj = cJSON_CreateObject();
	if (!obj) { free(results); json_error(buf, size, status, 500, "Internal error"); return; }
	cJSON_AddItemToObject(obj, "results", cJSON_CreateString(results));
	free(results);
	char *s = cJSON_PrintUnformatted(obj);
	cJSON_Delete(obj);
	if (s) {
		json_response(buf, size, status, s);
		free(s);
	} else {
		json_error(buf, size, status, 500, "Internal error");
	}
}

static void handle_sessions_list(char *buf, size_t size, int *status)
{
	char *ids[64];
	int n = session_list(ids, 64);
	cJSON *arr = cJSON_CreateArray();
	if (!arr) { json_error(buf, size, status, 500, "Internal error"); return; }
	for (int i = 0; i < n; i++) {
		cJSON_AddItemToArray(arr, cJSON_CreateString(ids[i]));
		free(ids[i]);
	}
	char *s = cJSON_PrintUnformatted(arr);
	cJSON_Delete(arr);
	if (s) {
		json_response(buf, size, status, s);
		free(s);
	} else {
		json_error(buf, size, status, 500, "Internal error");
	}
}

static void handle_session_delete(const char *id, char *buf, size_t size, int *status)
{
	if (session_delete(id) != 0) {
		json_error(buf, size, status, 404, "Session not found");
		return;
	}
	*status = 200;
	json_response(buf, size, status, "{\"ok\":true}");
}

static void handle_cron_list(char *buf, size_t size, int *status)
{
	cron_job_row_t *rows = calloc(64, sizeof(cron_job_row_t));
	if (!rows) { json_error(buf, size, status, 500, "Out of memory"); return; }
	int n = cron_job_list(rows, 64);
	cJSON *arr = cJSON_CreateArray();
	if (!arr) { free(rows); json_error(buf, size, status, 500, "Internal error"); return; }
	for (int i = 0; i < n; i++) {
		cJSON *obj = cJSON_CreateObject();
		if (!obj) {
			free(rows);
			cJSON_Delete(arr);
			json_error(buf, size, status, 500, "Internal error");
			return;
		}
		cJSON_AddItemToObject(obj, "id", cJSON_CreateString(rows[i].id));
		cJSON_AddItemToObject(obj, "schedule", cJSON_CreateString(rows[i].schedule));
		cJSON_AddItemToObject(obj, "message", cJSON_CreateString(rows[i].message));
		cJSON_AddItemToObject(obj, "channel", cJSON_CreateString(rows[i].channel));
		cJSON_AddItemToObject(obj, "recipient", cJSON_CreateString(rows[i].recipient));
		cJSON_AddItemToObject(obj, "next_run", cJSON_CreateNumber((double)rows[i].next_run));
		cJSON_AddItemToObject(obj, "enabled", cJSON_CreateBool(rows[i].enabled));
		cJSON_AddItemToArray(arr, obj);
	}
	free(rows);
	json_print_to_buf(arr, buf, size, status);
	cJSON_Delete(arr);
}

static void handle_cron_create(const char *body, size_t body_len, char *buf, size_t size, int *status)
{
	cJSON *root = body ? cJSON_ParseWithLength(body, body_len) : NULL;
	if (!root) { json_error(buf, size, status, 400, "Invalid JSON"); return; }
	cJSON *schedule = cJSON_GetObjectItem(root, "schedule");
	cJSON *message = cJSON_GetObjectItem(root, "message");
	if (!cJSON_IsString(schedule) || !cJSON_IsString(message)) {
		cJSON_Delete(root);
		json_error(buf, size, status, 400, "schedule and message required");
		return;
	}
	cJSON *id_node = cJSON_GetObjectItem(root, "id");
	cJSON *channel = cJSON_GetObjectItem(root, "channel");
	cJSON *recipient = cJSON_GetObjectItem(root, "recipient");
	char id[128];
	if (cJSON_IsString(id_node) && id_node->valuestring[0])
		snprintf(id, sizeof(id), "%.127s", id_node->valuestring);
	else
		id[0] = '\0';
	const char *ch = (channel && cJSON_IsString(channel)) ? channel->valuestring : NULL;
	const char *rec = (recipient && cJSON_IsString(recipient)) ? recipient->valuestring : NULL;
	int ret = cron_create_job(schedule->valuestring, message->valuestring, ch, rec, id, sizeof(id));
	cJSON_Delete(root);
	if (ret != 0) {
		json_error(buf, size, status, 400, "Failed to create job");
		return;
	}
	cJSON *resp_obj = cJSON_CreateObject();
	if (!resp_obj) {
		json_error(buf, size, status, 500, "Internal error");
		return;
	}
	cJSON_AddBoolToObject(resp_obj, "ok", 1);
	cJSON_AddStringToObject(resp_obj, "id", id);
	if (json_print_to_buf(resp_obj, buf, size, status) == 0)
		*status = 201;
	cJSON_Delete(resp_obj);
}

static void handle_cron_delete(const char *id, char *buf, size_t size, int *status)
{
	if (cron_job_delete(id) != 0) {
		json_error(buf, size, status, 404, "Job not found");
		return;
	}
	*status = 200;
	json_response(buf, size, status, "{\"ok\":true}");
}

static void handle_cron_toggle(const char *id, char *buf, size_t size, int *status)
{
	if (cron_job_toggle(id) != 0) {
		json_error(buf, size, status, 404, "Job not found");
		return;
	}
	*status = 200;
	json_response(buf, size, status, "{\"ok\":true}");
}

/** Build a minimal JSON-RPC 2.0 error response into @p buf. */
static void jsonrpc_error(char *buf, size_t size, int *status,
	int http_code, int rpc_code, const char *msg)
{
	cJSON *obj;
	cJSON *err;
	char *s;
	if (!buf || size == 0 || !status) return;
	*status = http_code;
	obj = cJSON_CreateObject();
	if (!obj) { snprintf(buf, size, "{\"error\":{\"code\":%d}}", rpc_code); return; }
	cJSON_AddStringToObject(obj, "jsonrpc", "2.0");
	cJSON_AddNullToObject(obj, "id");
	err = cJSON_CreateObject();
	if (err) {
		cJSON_AddNumberToObject(err, "code", (double)rpc_code);
		cJSON_AddStringToObject(err, "message", msg ? msg : "error");
		cJSON_AddItemToObject(obj, "error", err);
	}
	s = cJSON_PrintUnformatted(obj);
	cJSON_Delete(obj);
	if (s) {
		size_t len = strlen(s);
		if (len >= size) len = size - 1;
		memcpy(buf, s, len);
		buf[len] = '\0';
		free(s);
	}
}

static void handle_asap_log_get(char *buf, size_t size, int *status)
{
	asap_log_entry_t entries[ASAP_LOG_RING_SIZE];
	int n;
	int i;
	cJSON *root;
	cJSON *arr;
	char *s;
	struct tm tm;
	char tsbuf[40];
	if (!buf || size == 0 || !status) return;
	n = asap_log_get_snapshot(entries, ASAP_LOG_RING_SIZE);
	root = cJSON_CreateObject();
	if (!root) {
		json_error(buf, size, status, 500, "Internal error");
		return;
	}
	arr = cJSON_CreateArray();
	if (!arr) {
		cJSON_Delete(root);
		json_error(buf, size, status, 500, "Internal error");
		return;
	}
	cJSON_AddItemToObject(root, "entries", arr);
	for (i = 0; i < n; i++) {
		cJSON *e = cJSON_CreateObject();
		if (!e) continue;
		gmtime_r(&entries[i].ts, &tm);
		strftime(tsbuf, sizeof tsbuf, "%Y-%m-%dT%H:%M:%SZ", &tm);
		cJSON_AddStringToObject(e, "timestamp", tsbuf);
		cJSON_AddStringToObject(e, "direction",
			entries[i].direction == ASAP_LOG_DIR_IN ? "in" : "out");
		cJSON_AddStringToObject(e, "payload_type", entries[i].payload_type);
		cJSON_AddStringToObject(e, "id", entries[i].id);
		if (entries[i].json_snippet[0])
			cJSON_AddStringToObject(e, "snippet", entries[i].json_snippet);
		cJSON_AddItemToArray(arr, e);
	}
	s = cJSON_PrintUnformatted(root);
	cJSON_Delete(root);
	if (!s) {
		json_error(buf, size, status, 500, "Internal error");
		return;
	}
	json_response(buf, size, status, s);
	free(s);
}

static void handle_asap(http_server_ctx_t *ctx, const char *client_ip,
	const char *body, size_t body_len, char *buf, size_t size, int *status)
{
	asap_envelope_t in;
	asap_envelope_t out;
	asap_server_ctx_t asap_ctx;
	cJSON *parse_err = NULL;
	char err_msg[256];
	char *resp_json;
	char *snippet;
	int rc;
	(void)body_len;
	if (rate_limit_asap(client_ip, time(NULL))) {
		jsonrpc_error(buf, size, status, 429, -32000, "rate limit exceeded");
		return;
	}
	asap_envelope_init(&in);
	rc = asap_envelope_parse(body, &in, &parse_err);
	if (rc != 0) {
		char *err_str = parse_err ? cJSON_PrintUnformatted(parse_err) : NULL;
		cJSON_Delete(parse_err);
		asap_envelope_clear(&in);
		jsonrpc_error(buf, size, status, 400, -32700, err_str ? err_str : "parse error");
		free(err_str);
		return;
	}
	snippet = in.payload ? cJSON_PrintUnformatted(in.payload) : NULL;
	asap_log_append_in(in.payload_type, in.id, snippet);
	free(snippet);
	memset(&asap_ctx, 0, sizeof asap_ctx);
	asap_ctx.cfg = ctx ? ctx->cfg : NULL;
	err_msg[0] = '\0';
	asap_envelope_init(&out);
	rc = asap_server_handle(&in, &out, &asap_ctx, err_msg, sizeof err_msg);
	asap_envelope_clear(&in);
	if (rc != 0) {
		asap_envelope_clear(&out);
		jsonrpc_error(buf, size, status, 400, rc, err_msg[0] ? err_msg : "handler error");
		return;
	}
	resp_json = asap_envelope_to_jsonrpc_string(&out, NULL);
	snippet = out.payload ? cJSON_PrintUnformatted(out.payload) : NULL;
	asap_log_append_out(out.payload_type, out.id, snippet);
	free(snippet);
	asap_envelope_clear(&out);
	if (!resp_json) {
		jsonrpc_error(buf, size, status, 500, -32603, "failed to serialize response");
		return;
	}
	*status = 200;
	{
		size_t rlen = strlen(resp_json);
		if (rlen >= size) rlen = size - 1;
		memcpy(buf, resp_json, rlen);
		buf[rlen] = '\0';
	}
	free(resp_json);
}

static void handle_well_known(http_server_ctx_t *ctx, const char *uri, int uri_len,
                             char *buf, size_t size, int *status)
{
	if (uri_exact_eq(uri, uri_len, "/.well-known/asap/manifest.json")) {
		char keys_err[256] = {0};
		char *json;

		/* Keys are created at gateway bootstrap (init_subsystems), not here;
		 * this is a defensive re-check (idempotent + cheap when already
		 * loaded), kept so a gateway that skipped bootstrap keygen still
		 * fails closed rather than serving an unsigned manifest. The creation
		 * was moved off this unauthenticated path to prevent an attacker from
		 * forcing keypair creation + disk fsync by hitting a public route. */
		if (manifest_keys_ensure_loaded(keys_err, sizeof(keys_err)) != 0) {
			if (keys_err[0] != '\0')
				fprintf(stderr, "manifest keys: %s\n", keys_err);
			json_error(buf, size, status, 500, "Signing key unavailable");
			return;
		}
		json = manifest_build_signed_json(ctx ? ctx->cfg : NULL);
		if (!json) {
			json_error(buf, size, status, 500, "Internal error");
			return;
		}
		*status = 200;
		json_response(buf, size, status, json);
		free(json);
		return;
	}
	if (uri_exact_eq(uri, uri_len, "/.well-known/asap/health")) {
		*status = 200;
		json_response(buf, size, status, manifest_health_json());
		return;
	}
	json_error(buf, size, status, 404, "Not found");
}

static int extract_path_param(const char *uri, int uri_len, const char *prefix, char *out, size_t out_size)
{
	size_t plen = strlen(prefix);
	if (uri_len <= (int)plen) return -1;
	const char *rest = uri + plen;
	int rest_len = uri_len - (int)plen;
	const char *slash = memchr(rest, '/', (size_t)rest_len);
	int seg_len = slash ? (int)(slash - rest) : rest_len;
	if (seg_len <= 0 || (size_t)seg_len >= out_size) return -1;
	memcpy(out, rest, (size_t)seg_len);
	out[seg_len] = '\0';
	return 0;
}

static void handle_api_status(const config_t *cfg, char *buf, size_t size, int *status)
{
	char *prov_json = provider_router_api_status_json();
	cJSON *root;
	cJSON *discord_obj;
	discord_status_snapshot_t ds;
	char *merged;
	if (!prov_json) {
		json_error(buf, size, status, 500, "Internal error");
		return;
	}
	root = cJSON_Parse(prov_json);
	free(prov_json);
	if (!root) {
		json_error(buf, size, status, 500, "Internal error");
		return;
	}
	discord_status_snapshot_fill(cfg, &ds);
	discord_obj = cJSON_CreateObject();
	if (!discord_obj) {
		cJSON_Delete(root);
		json_error(buf, size, status, 500, "Internal error");
		return;
	}
	cJSON_AddItemToObject(discord_obj, "lifecycle", cJSON_CreateString(discord_lifecycle_str(ds.lifecycle)));
	if (ds.reason[0])
		cJSON_AddItemToObject(discord_obj, "reason", cJSON_CreateString(ds.reason));
	cJSON_AddItemToObject(root, "discord", discord_obj);
	merged = cJSON_PrintUnformatted(root);
	cJSON_Delete(root);
	if (!merged) {
		json_error(buf, size, status, 500, "Internal error");
		return;
	}
	json_response(buf, size, status, merged);
	free(merged);
}

static void handle_api_context_snapshot(char *buf, size_t size, int *status)
{
	char *snapshot;

	snapshot = tool_context_snapshot_json();
	if (!snapshot) {
		json_error(buf, size, status, 500, "Internal error");
		return;
	}
	json_response(buf, size, status, snapshot);
	free(snapshot);
}

int dispatch_route(http_server_ctx_t *ctx, struct lws *wsi, int method,
                          const char *uri, int uri_len, const char *body, size_t body_len,
                          char *buf, size_t size, int *status)
{
	if (uri_exact_eq(uri, uri_len, "/health")) {
		handle_health(ctx, buf, size, status);
		return 0;
	}
	if (uri_exact_eq(uri, uri_len, "/pair") && method == HTTP_POST) {
		char client_ip[64] = {0};
		lws_get_peer_simple(wsi, client_ip, sizeof client_ip);
		if (ctx && ctx->auth && auth_pair_check_lockout(ctx->auth, client_ip, time(NULL))) {
			json_error(buf, size, status, 429, "Too many failed attempts");
			return 0;
		}
		handle_pair(ctx, wsi, body, body_len, buf, size, status);
		if (ctx && ctx->auth) {
			if (*status == 200)
				auth_pair_clear_ip(ctx->auth, client_ip);
			else
				auth_pair_record_failure(ctx->auth, client_ip, time(NULL));
		}
		return 0;
	}
	if (uri_has_prefix(uri, uri_len, "/.well-known/")) {
		handle_well_known(ctx, uri, uri_len, buf, size, status);
		return 0;
	}
	if (uri_exact_eq(uri, uri_len, "/api/config")) {
		if (method == HTTP_GET) handle_config_get(ctx->cfg, buf, size, status);
		else if (method == HTTP_PUT) handle_config_put(ctx, body, body_len, buf, size, status);
		else json_error(buf, size, status, 405, "Method not allowed");
		return 0;
	}
	if (uri_exact_eq(uri, uri_len, "/api/status")) {
		if (method == HTTP_GET) handle_api_status(ctx->cfg, buf, size, status);
		else json_error(buf, size, status, 405, "Method not allowed");
		return 0;
	}
	if (uri_exact_eq(uri, uri_len, "/api/context/snapshot")) {
		if (method == HTTP_GET) handle_api_context_snapshot(buf, size, status);
		else json_error(buf, size, status, 405, "Method not allowed");
		return 0;
	}
	if (uri_exact_eq(uri, uri_len, "/api/skills")) {
		if (method == HTTP_GET) handle_skills_list(ctx->cfg, buf, size, status);
		else if (method == HTTP_POST) handle_skill_create(ctx->cfg, body, body_len, buf, size, status);
		else json_error(buf, size, status, 405, "Method not allowed");
		return 0;
	}
	if (uri_has_prefix(uri, uri_len, "/api/skills/")) {
		char name[128];
		if (extract_path_param(uri, uri_len, "/api/skills/", name, sizeof(name)) != 0) {
			json_error(buf, size, status, 404, "Not found");
			return 0;
		}
		if (method == HTTP_GET) handle_skill_get(ctx->cfg, name, buf, size, status);
		else if (method == HTTP_PUT) handle_skill_update(ctx->cfg, name, body, body_len, buf, size, status);
		else if (method == HTTP_DELETE) handle_skill_delete(ctx->cfg, name, buf, size, status);
		else json_error(buf, size, status, 405, "Method not allowed");
		return 0;
	}
	if (uri_has_prefix(uri, uri_len, "/api/memory")) {
		if (method != HTTP_GET) { json_error(buf, size, status, 405, "Method not allowed"); return 0; }
		char qbuf[256] = {0};
		char lbuf[32] = {0};
		lws_get_urlarg_by_name_safe(wsi, "q", qbuf, sizeof(qbuf));
		lws_get_urlarg_by_name_safe(wsi, "limit", lbuf, sizeof(lbuf));
		int limit = parse_memory_limit_arg(lbuf[0] ? lbuf : NULL);
		handle_memory_get(qbuf[0] ? qbuf : NULL, limit, buf, size, status);
		return 0;
	}
	if (uri_exact_eq(uri, uri_len, "/api/sessions")) {
		if (method == HTTP_GET) handle_sessions_list(buf, size, status);
		else json_error(buf, size, status, 405, "Method not allowed");
		return 0;
	}
	if (uri_has_prefix(uri, uri_len, "/api/sessions/")) {
		if (method != HTTP_DELETE) { json_error(buf, size, status, 405, "Method not allowed"); return 0; }
		char id[128];
		if (extract_path_param(uri, uri_len, "/api/sessions/", id, sizeof(id)) != 0) {
			json_error(buf, size, status, 404, "Not found");
			return 0;
		}
		handle_session_delete(id, buf, size, status);
		return 0;
	}
	if (uri_exact_eq(uri, uri_len, "/api/cron")) {
		if (method == HTTP_GET) handle_cron_list(buf, size, status);
		else if (method == HTTP_POST) handle_cron_create(body, body_len, buf, size, status);
		else json_error(buf, size, status, 405, "Method not allowed");
		return 0;
	}
	if (uri_has_prefix(uri, uri_len, "/api/cron/")) {
		char id[128];
		if (extract_path_param(uri, uri_len, "/api/cron/", id, sizeof(id)) != 0) {
			json_error(buf, size, status, 404, "Not found");
			return 0;
		}
		size_t suffix = strlen("/api/cron/") + strlen(id);
		/* Exact length: /toggle is 7 chars. The prior >= suffix+8 guard made
		 * POST /toggle (uri_len == suffix+7) fall through to the DELETE branch
		 * and always return 405, so the toggle endpoint was completely broken.
		 * Exact match also rejects trailing garbage like /<id>/toggle/extra. */
		int is_toggle = (uri_len == (int)(suffix + 7) &&
		                 strncmp(uri + suffix, "/toggle", 7) == 0);
		if (is_toggle) {
			if (method == HTTP_POST) handle_cron_toggle(id, buf, size, status);
			else json_error(buf, size, status, 405, "Method not allowed");
		} else {
			if (method == HTTP_DELETE) handle_cron_delete(id, buf, size, status);
			else json_error(buf, size, status, 405, "Method not allowed");
		}
		return 0;
	}
	if (uri_exact_eq(uri, uri_len, "/api/asap/log")) {
		if (method == HTTP_GET) handle_asap_log_get(buf, size, status);
		else json_error(buf, size, status, 405, "Method not allowed");
		return 0;
	}
	if (routes_hardware_dispatch(ctx, wsi, method, uri, uri_len, buf, size, status))
		return 0;
	if (uri_exact_eq(uri, uri_len, "/asap") && method == HTTP_POST) {
		char client_ip[64] = {0};
		lws_get_peer_simple(wsi, client_ip, sizeof client_ip);
		handle_asap(ctx, client_ip, body, body_len, buf, size, status);
		return 0;
	}
	json_error(buf, size, status, 404, "Not found");
	return 0;
}
