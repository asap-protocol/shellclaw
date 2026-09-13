/**
 * @file envelope.c
 * @brief ASAP envelope lifecycle and JSON-RPC error object builder.
 */
#define _POSIX_C_SOURCE 200809L

#include "asap/envelope.h"
#include <stdio.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

void asap_envelope_init(asap_envelope_t *env)
{
	if (!env) return;
	memset(env, 0, sizeof(*env));
}

static void str_free(char **p)
{
	if (p && *p) {
		free(*p);
		*p = NULL;
	}
}

void asap_envelope_clear(asap_envelope_t *env)
{
	if (!env) return;
	if (env->jsonrpc_request_id) {
		cJSON_Delete(env->jsonrpc_request_id);
		env->jsonrpc_request_id = NULL;
	}
	str_free(&env->id);
	str_free(&env->asap_version);
	str_free(&env->sender);
	str_free(&env->recipient);
	str_free(&env->payload_type);
	if (env->payload) {
		cJSON_Delete(env->payload);
		env->payload = NULL;
	}
	str_free(&env->correlation_id);
	str_free(&env->trace_id);
	str_free(&env->timestamp);
}

cJSON *asap_jsonrpc_error(int code, const char *message, cJSON *id)
{
	if (!message) return NULL;
	cJSON *root = cJSON_CreateObject();
	if (!root) return NULL;
	cJSON_AddStringToObject(root, "jsonrpc", "2.0");
	cJSON *err = cJSON_CreateObject();
	if (!err) { cJSON_Delete(root); return NULL; }
	if (!cJSON_AddNumberToObject(err, "code", (double)code)) {
		cJSON_Delete(err);
		cJSON_Delete(root);
		return NULL;
	}
	if (!cJSON_AddStringToObject(err, "message", message)) {
		cJSON_Delete(err);
		cJSON_Delete(root);
		return NULL;
	}
	cJSON_AddItemToObject(root, "error", err);
	if (id) {
		cJSON *id_copy = cJSON_Duplicate(id, 1);
		if (!id_copy) { cJSON_Delete(root); return NULL; }
		cJSON_AddItemToObject(root, "id", id_copy);
	} else {
		if (!cJSON_AddNullToObject(root, "id")) {
			cJSON_Delete(root);
			return NULL;
		}
	}
	return root;
}

/** ASAP v2.1 payload_type values (PRD §4.1). */
static int payload_type_is_allowed(const char *t)
{
	static const char *const allowed[] = {
		"task.request", "task.response", "task.cancel",
		"state.query", "mcp.tool_call", "mcp.tool_result"
	};
	if (!t) return 0;
	for (size_t i = 0; i < sizeof(allowed) / sizeof(allowed[0]); i++) {
		if (strcmp(t, allowed[i]) == 0) return 1;
	}
	return 0;
}

static int parse_fail(asap_envelope_t *out, cJSON *rpc_id_owned, cJSON **err_out, const char *msg)
{
	asap_envelope_clear(out);
	asap_envelope_init(out);
	if (err_out && msg) {
		cJSON *e = asap_jsonrpc_error(-32602, msg, rpc_id_owned);
		*err_out = e;
	} else {
		(void)err_out;
	}
	if (rpc_id_owned) cJSON_Delete(rpc_id_owned);
	return -1;
}

static cJSON *dup_request_id(cJSON *root)
{
	cJSON *id_item = cJSON_GetObjectItemCaseSensitive(root, "id");
	if (cJSON_HasObjectItem(root, "id")) {
		cJSON *d = cJSON_Duplicate(id_item, 1);
		return d ? d : NULL;
	}
	return cJSON_CreateNull();
}

static int require_str_in_params(cJSON *params, const char *key, char **out, cJSON *rpc_id,
		asap_envelope_t *env, cJSON **err_out)
{
	cJSON *j = cJSON_GetObjectItemCaseSensitive(params, key);
	if (!j || !cJSON_IsString(j) || !j->valuestring) {
		char errbuf[120];
		snprintf(errbuf, sizeof errbuf, "Invalid params: missing or bad type for '%.48s'", key);
		return parse_fail(env, rpc_id, err_out, errbuf);
	}
	*out = strdup(j->valuestring);
	if (!*out) return parse_fail(env, rpc_id, err_out, "Invalid params: allocation failed");
	return 0;
}

static int optional_str_in_params(cJSON *params, const char *key, char **out, cJSON *rpc_id,
		asap_envelope_t *env, cJSON **err_out)
{
	*out = NULL;
	if (!cJSON_HasObjectItem(params, key)) return 0;
	cJSON *j = cJSON_GetObjectItemCaseSensitive(params, key);
	if (!cJSON_IsString(j) || !j->valuestring)
		return parse_fail(env, rpc_id, err_out, "Invalid params: optional field has wrong type");
	*out = strdup(j->valuestring);
	if (!*out) return parse_fail(env, rpc_id, err_out, "Invalid params: allocation failed");
	return 0;
}

int asap_envelope_from_object(const cJSON *obj, cJSON *rpc_id, asap_envelope_t *out, cJSON **err_out)
{
	cJSON *r = (cJSON *)obj;
	cJSON *rid_temp = NULL;

	if (err_out) *err_out = NULL;
	if (!obj || !cJSON_IsObject(r) || !out) return -1; /* caller still owns rpc_id */
	if (!rpc_id) {
		rid_temp = cJSON_CreateNull();
		if (!rid_temp) return -1;
		rpc_id = rid_temp;
	}
	asap_envelope_clear(out);
	asap_envelope_init(out);
	if (require_str_in_params(r, "id", &out->id, rpc_id, out, err_out) != 0) return -1;
	if (require_str_in_params(r, "asap_version", &out->asap_version, rpc_id, out, err_out) != 0) return -1;
	if (require_str_in_params(r, "sender", &out->sender, rpc_id, out, err_out) != 0) return -1;
	if (require_str_in_params(r, "recipient", &out->recipient, rpc_id, out, err_out) != 0) return -1;
	if (require_str_in_params(r, "payload_type", &out->payload_type, rpc_id, out, err_out) != 0) return -1;
	if (!payload_type_is_allowed(out->payload_type))
		return parse_fail(out, rpc_id, err_out, "Invalid params: unknown payload_type");
	{
		cJSON *pl = cJSON_GetObjectItemCaseSensitive(r, "payload");
		if (pl == NULL || cJSON_IsNull(pl))
			return parse_fail(out, rpc_id, err_out, "Invalid params: missing or null 'payload'");
		out->payload = cJSON_Duplicate(pl, 1);
		if (!out->payload) return parse_fail(out, rpc_id, err_out, "Invalid params: allocation failed");
	}
	if (optional_str_in_params(r, "correlation_id", &out->correlation_id, rpc_id, out, err_out) != 0) return -1;
	if (optional_str_in_params(r, "trace_id", &out->trace_id, rpc_id, out, err_out) != 0) return -1;
	if (optional_str_in_params(r, "timestamp", &out->timestamp, rpc_id, out, err_out) != 0) return -1;
	if (rid_temp) cJSON_Delete(rid_temp);
	return 0;
}

/** Envelope key/value object for params or result (same shape). */
static cJSON *envelope_fields_cjson(const asap_envelope_t *env)
{
	cJSON *o;
	if (!env) return NULL;
	if (!env->id || !env->asap_version || !env->sender || !env->recipient || !env->payload_type) return NULL;
	if (!env->payload || cJSON_IsNull(env->payload)) return NULL;
	if (!payload_type_is_allowed(env->payload_type)) return NULL;
	o = cJSON_CreateObject();
	if (!o) return NULL;
	if (!cJSON_AddStringToObject(o, "id", env->id) || !cJSON_AddStringToObject(o, "asap_version", env->asap_version) ||
		!cJSON_AddStringToObject(o, "sender", env->sender) || !cJSON_AddStringToObject(o, "recipient", env->recipient) ||
		!cJSON_AddStringToObject(o, "payload_type", env->payload_type)) {
		cJSON_Delete(o);
		return NULL;
	}
	{
		cJSON *pl = cJSON_Duplicate(env->payload, 1);
		if (!pl) { cJSON_Delete(o); return NULL; }
		if (!cJSON_AddItemToObject(o, "payload", pl)) {
			cJSON_Delete(pl);
			cJSON_Delete(o);
			return NULL;
		}
	}
	if (env->correlation_id) {
		if (!cJSON_AddStringToObject(o, "correlation_id", env->correlation_id)) { cJSON_Delete(o); return NULL; }
	}
	if (env->trace_id) {
		if (!cJSON_AddStringToObject(o, "trace_id", env->trace_id)) { cJSON_Delete(o); return NULL; }
	}
	if (env->timestamp) {
		if (!cJSON_AddStringToObject(o, "timestamp", env->timestamp)) { cJSON_Delete(o); return NULL; }
	}
	return o;
}

cJSON *asap_envelope_to_jsonrpc(const asap_envelope_t *env, cJSON *jsonrpc_id)
{
	cJSON *id_src = jsonrpc_id ? jsonrpc_id : env->jsonrpc_request_id;
	cJSON *result = envelope_fields_cjson(env);
	cJSON *root;
	if (!result) return NULL;
	root = cJSON_CreateObject();
	if (!root) { cJSON_Delete(result); return NULL; }
	if (!cJSON_AddStringToObject(root, "jsonrpc", "2.0") || !cJSON_AddItemToObject(root, "result", result)) {
		cJSON_Delete(result);
		cJSON_Delete(root);
		return NULL;
	}
	if (id_src) {
		cJSON *idcp = cJSON_Duplicate(id_src, 1);
		if (!idcp) { cJSON_Delete(root); return NULL; }
		if (!cJSON_AddItemToObject(root, "id", idcp)) { cJSON_Delete(idcp); cJSON_Delete(root); return NULL; }
	} else {
		if (!cJSON_AddNullToObject(root, "id")) { cJSON_Delete(root); return NULL; }
	}
	return root;
}

char *asap_envelope_to_jsonrpc_string(const asap_envelope_t *env, cJSON *jsonrpc_id)
{
	cJSON *root = asap_envelope_to_jsonrpc(env, jsonrpc_id);
	if (!root) return NULL;
	char *s = cJSON_PrintUnformatted(root);
	cJSON_Delete(root);
	return s;
}

cJSON *asap_envelope_to_jsonrpc_request(const asap_envelope_t *env, cJSON *jsonrpc_id, const char *method)
{
	cJSON *params = envelope_fields_cjson(env);
	cJSON *root;
	const char *m = (method && method[0] != '\0') ? method : "asap.send";
	cJSON *id_src = jsonrpc_id ? jsonrpc_id : env->jsonrpc_request_id;
	if (!params) return NULL;
	root = cJSON_CreateObject();
	if (!root) { cJSON_Delete(params); return NULL; }
	if (!cJSON_AddStringToObject(root, "jsonrpc", "2.0") || !cJSON_AddStringToObject(root, "method", m) ||
		!cJSON_AddItemToObject(root, "params", params)) {
		cJSON_Delete(params);
		cJSON_Delete(root);
		return NULL;
	}
	if (id_src) {
		cJSON *idcp = cJSON_Duplicate(id_src, 1);
		if (!idcp) { cJSON_Delete(root); return NULL; }
		if (!cJSON_AddItemToObject(root, "id", idcp)) { cJSON_Delete(idcp); cJSON_Delete(root); return NULL; }
	} else {
		if (!cJSON_AddNullToObject(root, "id")) { cJSON_Delete(root); return NULL; }
	}
	return root;
}

char *asap_envelope_to_jsonrpc_request_string(const asap_envelope_t *env, cJSON *jsonrpc_id, const char *method)
{
	cJSON *root = asap_envelope_to_jsonrpc_request(env, jsonrpc_id, method);
	if (!root) return NULL;
	char *s = cJSON_PrintUnformatted(root);
	cJSON_Delete(root);
	return s;
}

int asap_envelope_parse_jsonrpc_response(const char *json, asap_envelope_t *out, char *errmsg, size_t errlen)
{
	if (errmsg && errlen) errmsg[0] = '\0';
	if (!json || !out) return -1;
	asap_envelope_init(out);
	cJSON *root = cJSON_Parse(json);
	if (!root) {
		if (errmsg && errlen) (void)snprintf(errmsg, errlen, "Invalid JSON in response");
		return -1;
	}
	cJSON *jrpc = cJSON_GetObjectItemCaseSensitive(root, "jsonrpc");
	if (!cJSON_IsString(jrpc) || !jrpc->valuestring || strcmp(jrpc->valuestring, "2.0") != 0) {
		if (errmsg && errlen) (void)snprintf(errmsg, errlen, "Invalid response: jsonrpc");
		cJSON_Delete(root);
		return -1;
	}
	{
		cJSON *er = cJSON_GetObjectItemCaseSensitive(root, "error");
		if (er && cJSON_IsObject(er)) {
			cJSON *em = cJSON_GetObjectItemCaseSensitive(er, "message");
			const char *e = cJSON_IsString(em) ? em->valuestring : "JSON-RPC error";
			if (errmsg && errlen) (void)snprintf(errmsg, errlen, "%s", e);
			cJSON_Delete(root);
			return -1;
		}
	}
	{
		cJSON *res = cJSON_GetObjectItemCaseSensitive(root, "result");
		cJSON *rpc_id;
		cJSON *tmp_err = NULL;
		if (!res || !cJSON_IsObject(res)) {
			if (errmsg && errlen) (void)snprintf(errmsg, errlen, "Invalid response: missing result");
			cJSON_Delete(root);
			return -1;
		}
		rpc_id = dup_request_id(root);
		if (!rpc_id) {
			cJSON_Delete(root);
			if (errmsg && errlen) (void)snprintf(errmsg, errlen, "Out of memory");
			return -1;
		}
		if (asap_envelope_from_object(res, rpc_id, out, &tmp_err) != 0) {
			if (tmp_err) {
				cJSON *e = cJSON_GetObjectItem(tmp_err, "error");
				cJSON *m = e ? cJSON_GetObjectItem(e, "message") : NULL;
				if (errmsg && errlen && cJSON_IsString(m) && m->valuestring) (void)snprintf(errmsg, errlen, "%s", m->valuestring);
				cJSON_Delete(tmp_err);
			} else if (errmsg && errlen) (void)snprintf(errmsg, errlen, "Invalid result envelope");
			/* parse_fail already freed rpc_id; inbound parse does not delete again (Refs: #84). */
			cJSON_Delete(root);
			return -1;
		}
		out->jsonrpc_request_id = rpc_id;
		cJSON_Delete(root);
		return 0;
	}
}

int asap_envelope_parse(const char *json, asap_envelope_t *out, cJSON **err_out)
{
	if (err_out) *err_out = NULL;
	if (!json || !out) return -1;
	asap_envelope_init(out);
	cJSON *root = cJSON_Parse(json);
	if (!root) {
		if (err_out) {
			cJSON *null_id = cJSON_CreateNull();
			if (null_id) {
				*err_out = asap_jsonrpc_error(-32700, "Parse error", null_id);
				cJSON_Delete(null_id);
			}
		}
		return -1;
	}
	cJSON *rpc_id = dup_request_id(root);
	if (!rpc_id) {
		cJSON_Delete(root);
		return -1;
	}
	cJSON *jrpc = cJSON_GetObjectItemCaseSensitive(root, "jsonrpc");
	if (!cJSON_IsString(jrpc) || !jrpc->valuestring || strcmp(jrpc->valuestring, "2.0") != 0) {
		cJSON_Delete(root);
		return parse_fail(out, rpc_id, err_out, "Invalid Request: jsonrpc must be \"2.0\"");
	}
	cJSON *meth = cJSON_GetObjectItemCaseSensitive(root, "method");
	if (!cJSON_IsString(meth) || !meth->valuestring || meth->valuestring[0] == '\0') {
		cJSON_Delete(root);
		return parse_fail(out, rpc_id, err_out, "Invalid Request: method must be a non-empty string");
	}
	cJSON *params = cJSON_GetObjectItemCaseSensitive(root, "params");
	if (!params || !cJSON_IsObject(params)) {
		cJSON_Delete(root);
		return parse_fail(out, rpc_id, err_out, "Invalid Request: params must be an object");
	}
	if (asap_envelope_from_object(params, rpc_id, out, err_out) != 0) {
		cJSON_Delete(root);
		return -1;
	}
	out->jsonrpc_request_id = rpc_id;
	cJSON_Delete(root);
	return 0;
}
