/**
 * @file test_gateway_http.c
 * @brief Integration tests for gateway HTTP: health, pair, auth, manifest, config, skills, memory, cron.
 * Requires libwebsockets and SHELLCLAW_GATEWAY. Starts server in subprocess.
 */
#define _POSIX_C_SOURCE 200809L

#include "gateway/auth.h"
#include "gateway/rate_limit.h"
#include "core/config.h"
#include "core/version.h"
#include "cJSON.h"
#include <curl/curl.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#ifndef INADDR_LOOPBACK
#define INADDR_LOOPBACK ((in_addr_t)0x7f000001)
#endif

#define ASSERT(c) do { if (!(c)) { fprintf(stderr, "FAIL: %s:%d %s\n", __FILE__, __LINE__, #c); return 1; } } while (0)

static char g_test_home[64];
static char g_base_url[128];
static char g_url_buf[512];

static const char *gw_url(const char *path)
{
	snprintf(g_url_buf, sizeof(g_url_buf), "%s%s", g_base_url, path);
	return g_url_buf;
}

static int pick_ephemeral_port(void)
{
	int fd;
	struct sockaddr_in addr;
	socklen_t len = sizeof(addr);
	fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) return -1;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	addr.sin_port = 0;
	if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
		close(fd);
		return -1;
	}
	if (getsockname(fd, (struct sockaddr *)&addr, &len) != 0) {
		close(fd);
		return -1;
	}
	close(fd);
	return (int)ntohs(addr.sin_port);
}

static int http_get(const char *url, long *code_out, char **body_out);
static int http_post_raw(const char *url, const void *data, size_t data_len,
			 const char *content_length, long *code_out, char **body_out);
static int http_post(const char *url, const char *json, long *code_out, char **body_out);

static int wait_for_health(int max_attempts)
{
	long code;
	char *body = NULL;
	int i;
	for (i = 0; i < max_attempts; i++) {
		body = NULL;
		if (http_get(gw_url("/health"), &code, &body) == 0 && code == 200) {
			free(body);
			return 0;
		}
		free(body);
		{
			struct timespec delay = { 0, 200000000L };
			(void)nanosleep(&delay, NULL);
		}
	}
	return -1;
}

static size_t write_cb(char *ptr, size_t size, size_t nmemb, void *user)
{
	size_t total = size * nmemb;
	char **buf = (char **)user;
	size_t prev = *buf ? strlen(*buf) : 0;
	char *new_buf = realloc(*buf, prev + total + 1);
	if (!new_buf) return 0;
	*buf = new_buf;
	memcpy(new_buf + prev, ptr, total);
	new_buf[prev + total] = '\0';
	return total;
}

static int http_get(const char *url, long *code_out, char **body_out)
{
	CURL *curl = curl_easy_init();
	if (!curl) return -1;
	*body_out = NULL;
	curl_easy_setopt(curl, CURLOPT_URL, url);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, body_out);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);
	CURLcode res = curl_easy_perform(curl);
	long code = 0;
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
	curl_easy_cleanup(curl);
	if (code_out) *code_out = code;
	return (res == CURLE_OK) ? 0 : -1;
}

static int http_post(const char *url, const char *json, long *code_out, char **body_out)
{
	return http_post_raw(url, json, json ? strlen(json) : 0, NULL, code_out, body_out);
}

static int http_post_raw(const char *url, const void *data, size_t data_len,
			 const char *content_length, long *code_out, char **body_out)
{
	CURL *curl = curl_easy_init();
	char cl_hdr[64];
	if (!curl) return -1;
	*body_out = NULL;
	struct curl_slist *headers = NULL;
	headers = curl_slist_append(headers, "Content-Type: application/json");
	if (content_length && content_length[0] != '\0') {
		snprintf(cl_hdr, sizeof(cl_hdr), "Content-Length: %s", content_length);
		headers = curl_slist_append(headers, cl_hdr);
	}
	curl_easy_setopt(curl, CURLOPT_URL, url);
	curl_easy_setopt(curl, CURLOPT_POSTFIELDS, data);
	curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)data_len);
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, body_out);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);
	CURLcode res = curl_easy_perform(curl);
	long code = 0;
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
	curl_slist_free_all(headers);
	curl_easy_cleanup(curl);
	if (code_out) *code_out = code;
	return (res == CURLE_OK) ? 0 : -1;
}

static int http_get_auth(const char *url, const char *bearer, long *code_out, char **body_out)
{
	CURL *curl = curl_easy_init();
	if (!curl) return -1;
	*body_out = NULL;
	struct curl_slist *headers = NULL;
	char auth_hdr[256];
	snprintf(auth_hdr, sizeof(auth_hdr), "Authorization: Bearer %s", bearer);
	headers = curl_slist_append(headers, auth_hdr);
	curl_easy_setopt(curl, CURLOPT_URL, url);
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, body_out);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);
	CURLcode res = curl_easy_perform(curl);
	long code = 0;
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
	curl_slist_free_all(headers);
	curl_easy_cleanup(curl);
	if (code_out) *code_out = code;
	return (res == CURLE_OK) ? 0 : -1;
}

static int http_post_auth(const char *url, const char *bearer, const char *json, long *code_out, char **body_out)
{
	CURL *curl = curl_easy_init();
	if (!curl) return -1;
	*body_out = NULL;
	struct curl_slist *headers = NULL;
	char auth_hdr[256];
	snprintf(auth_hdr, sizeof(auth_hdr), "Authorization: Bearer %s", bearer);
	headers = curl_slist_append(headers, auth_hdr);
	headers = curl_slist_append(headers, "Content-Type: application/json");
	curl_easy_setopt(curl, CURLOPT_URL, url);
	curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json);
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, body_out);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);
	CURLcode res = curl_easy_perform(curl);
	long code = 0;
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
	curl_slist_free_all(headers);
	curl_easy_cleanup(curl);
	if (code_out) *code_out = code;
	return (res == CURLE_OK) ? 0 : -1;
}

static int http_put_auth(const char *url, const char *bearer, const char *body,
			 long *code_out, char **body_out)
{
	CURL *curl = curl_easy_init();
	if (!curl) return -1;
	*body_out = NULL;
	struct curl_slist *headers = NULL;
	char auth_hdr[256];
	snprintf(auth_hdr, sizeof(auth_hdr), "Authorization: Bearer %s", bearer);
	headers = curl_slist_append(headers, auth_hdr);
	headers = curl_slist_append(headers, "Content-Type: application/json");
	curl_easy_setopt(curl, CURLOPT_URL, url);
	curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PUT");
	curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, body_out);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);
	CURLcode res = curl_easy_perform(curl);
	long code = 0;
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
	curl_slist_free_all(headers);
	curl_easy_cleanup(curl);
	if (code_out) *code_out = code;
	return (res == CURLE_OK) ? 0 : -1;
}

static int http_delete_auth(const char *url, const char *bearer, long *code_out, char **body_out)
{
	CURL *curl = curl_easy_init();
	if (!curl) return -1;
	*body_out = NULL;
	struct curl_slist *headers = NULL;
	char auth_hdr[256];
	snprintf(auth_hdr, sizeof(auth_hdr), "Authorization: Bearer %s", bearer);
	headers = curl_slist_append(headers, auth_hdr);
	curl_easy_setopt(curl, CURLOPT_URL, url);
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
	curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "DELETE");
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, body_out);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);
	CURLcode res = curl_easy_perform(curl);
	long code = 0;
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
	curl_slist_free_all(headers);
	curl_easy_cleanup(curl);
	if (code_out) *code_out = code;
	return (res == CURLE_OK) ? 0 : -1;
}

static int read_pairing_code_from_file(const char *home, char *out, size_t out_sz)
{
	char path[160];
	FILE *f;
	if (!home || !out || out_sz < 7)
		return -1;
	snprintf(path, sizeof(path), "%s/.shellclaw/test_pairing_code", home);
	for (int i = 0; i < 50; i++) {
		f = fopen(path, "r");
		if (f) {
			if (fscanf(f, "%6[0-9]", out) == 1) {
				out[6] = '\0';
				fclose(f);
				return 0;
			}
			fclose(f);
		}
		{
			struct timespec delay = { 0, 100000000L };
			(void)nanosleep(&delay, NULL);
		}
	}
	return -1;
}

static int test_health(void)
{
	long code;
	char *body = NULL;
	int r = http_get(gw_url("/health"), &code, &body);
	ASSERT(r == 0);
	ASSERT(code == 200);
	ASSERT(body != NULL);
	ASSERT(strstr(body, "ok") != NULL);
	ASSERT(strstr(body, "uptime") != NULL);
	ASSERT(strstr(body, SHELLCLAW_RELEASE_VERSION) != NULL);
	free(body);
	return 0;
}

static int test_pair(const char *pairing_code, char *token_out, size_t token_size)
{
	if (!pairing_code || !token_out || token_size == 0) return 1;
	char post_json[128];
	snprintf(post_json, sizeof(post_json), "{\"code\":\"%s\"}", pairing_code);
	long code_http;
	char *body = NULL;
	CURL *curl = curl_easy_init();
	if (!curl) return 1;
	struct curl_slist *headers = NULL;
	char pair_hdr[64];
	headers = curl_slist_append(headers, "Content-Type: application/json");
	snprintf(pair_hdr, sizeof(pair_hdr), "X-Pairing-Code: %s", pairing_code);
	headers = curl_slist_append(headers, pair_hdr);
	curl_easy_setopt(curl, CURLOPT_URL, gw_url("/pair"));
	curl_easy_setopt(curl, CURLOPT_POSTFIELDS, post_json);
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);
	CURLcode res = curl_easy_perform(curl);
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code_http);
	curl_slist_free_all(headers);
	curl_easy_cleanup(curl);
	if (res != CURLE_OK) {
		free(body);
		return 1;
	}
	if (code_http != 200) {
		fprintf(stderr, "test_pair: HTTP %ld body=%s code=%s\n",
		        code_http, body ? body : "(null)", pairing_code);
		free(body);
		return 1;
	}
	ASSERT(body != NULL);
	ASSERT(strstr(body, "token") != NULL);
	cJSON *root = cJSON_Parse(body);
	ASSERT(root != NULL);
	cJSON *tok = cJSON_GetObjectItem(root, "token");
	ASSERT(tok != NULL && cJSON_IsString(tok));
	size_t len = strlen(tok->valuestring);
	if (len >= token_size) len = token_size - 1;
	memcpy(token_out, tok->valuestring, len);
	token_out[len] = '\0';
	cJSON_Delete(root);
	free(body);
	return 0;
}

static int test_api_config_401(void)
{
	long code;
	char *body = NULL;
	int r = http_get(gw_url("/api/config"), &code, &body);
	ASSERT(r == 0);
	ASSERT(code == 401);
	free(body);
	return 0;
}

static int test_api_config_invalid_bearer(const char *valid_token)
{
	long code;
	char *body = NULL;
	int r = http_get_auth(gw_url("/api/config"), "not-a-valid-paired-token", &code, &body);
	(void)valid_token;
	ASSERT(r == 0);
	ASSERT(code == 401);
	free(body);
	return 0;
}

static int test_api_config_401_malformed_auth(void)
{
	long code;
	char *body = NULL;
	CURL *curl = curl_easy_init();

	if (!curl)
		return 1;
	struct curl_slist *headers = NULL;
	headers = curl_slist_append(headers, "Authorization: not-bearer-format");
	curl_easy_setopt(curl, CURLOPT_URL, gw_url("/api/config"));
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
	if (curl_easy_perform(curl) != CURLE_OK) {
		curl_slist_free_all(headers);
		curl_easy_cleanup(curl);
		free(body);
		return 1;
	}
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
	curl_slist_free_all(headers);
	curl_easy_cleanup(curl);
	ASSERT(code == 401);
	free(body);
	return 0;
}

static int test_asap_invalid_body(void)
{
	long code;
	char *body = NULL;
	int r = http_post(gw_url("/asap"), "not-json", &code, &body);
	ASSERT(r == 0);
	ASSERT(code == 400);
	ASSERT(body != NULL);
	ASSERT(strstr(body, "error") != NULL);
	free(body);
	return 0;
}

static int test_asap_missing_fields(void)
{
	long code;
	char *body = NULL;
	int r = http_post(gw_url("/asap"), "{\"jsonrpc\":\"2.0\"}", &code, &body);
	ASSERT(r == 0);
	ASSERT(code == 400);
	ASSERT(body != NULL);
	ASSERT(strstr(body, "error") != NULL);
	free(body);
	return 0;
}

static int post_asap(const char *payload_type, const char *payload_json,
	const char *env_id, long *code, char **body)
{
	char req[1536];
	int n;
	n = snprintf(req, sizeof req,
		"{\"jsonrpc\":\"2.0\",\"method\":\"asap.send\","
		"\"params\":{"
		"\"id\":\"%s\","
		"\"asap_version\":\"2.1\","
		"\"sender\":\"urn:asap:agent:a\","
		"\"recipient\":\"urn:asap:agent:b\","
		"\"payload_type\":\"%s\","
		"\"payload\":%s,"
		"\"correlation_id\":\"c1\","
		"\"trace_id\":\"t1\","
		"\"timestamp\":\"2026-01-01T00:00:00Z\""
		"},\"id\":42}",
		env_id, payload_type, payload_json);
	if (n < 0 || (size_t)n >= sizeof req)
		return -1;
	return http_post(gw_url("/asap"), req, code, body);
}

static int asap_http_is(long code, long want, const char *body)
{
	if (code == want)
		return 1;
	fprintf(stderr, "FAIL: HTTP %ld want %ld body=%s\n",
		code, want, body ? body : "(null)");
	return 0;
}

static int asap_result_payload_type_is(const char *body, const char *want)
{
	cJSON *root = cJSON_Parse(body);
	cJSON *result;
	cJSON *ptype;
	int ok = 0;
	if (!root)
		return 0;
	result = cJSON_GetObjectItemCaseSensitive(root, "result");
	ptype = result ? cJSON_GetObjectItemCaseSensitive(result, "payload_type") : NULL;
	if (ptype && cJSON_IsString(ptype) && ptype->valuestring &&
	    strcmp(ptype->valuestring, want) == 0)
		ok = 1;
	cJSON_Delete(root);
	return ok;
}

static int asap_error_code_is(const char *body, int want)
{
	cJSON *root = cJSON_Parse(body);
	cJSON *err;
	cJSON *code;
	int ok = 0;
	if (!root)
		return 0;
	err = cJSON_GetObjectItemCaseSensitive(root, "error");
	code = err ? cJSON_GetObjectItemCaseSensitive(err, "code") : NULL;
	if (code && cJSON_IsNumber(code) && (int)code->valuedouble == want)
		ok = 1;
	cJSON_Delete(root);
	return ok;
}

static int asap_mcp_result_is_json_array(const char *body)
{
	cJSON *root = cJSON_Parse(body);
	cJSON *result;
	cJSON *payload;
	cJSON *result_str;
	cJSON *inner = NULL;
	int ok = 0;
	if (!root)
		return 0;
	result = cJSON_GetObjectItemCaseSensitive(root, "result");
	payload = result ? cJSON_GetObjectItemCaseSensitive(result, "payload") : NULL;
	result_str = payload ? cJSON_GetObjectItemCaseSensitive(payload, "result") : NULL;
	if (result_str && cJSON_IsString(result_str) && result_str->valuestring) {
		inner = cJSON_Parse(result_str->valuestring);
		ok = (inner && cJSON_IsArray(inner)) ? 1 : 0;
		cJSON_Delete(inner);
	}
	cJSON_Delete(root);
	return ok;
}

static int test_asap_task_request(void)
{
	long code;
	char *body = NULL;
	int r = post_asap("task.request", "{\"input\":\"hello\"}", "01HZABC123",
		&code, &body);
	ASSERT(r == 0);
	ASSERT(asap_http_is(code, 200, body));
	ASSERT(body != NULL);
	ASSERT(asap_result_payload_type_is(body, "task.response"));
	free(body);
	return 0;
}

static int test_asap_mcp_tool_call(void)
{
	long code;
	char *body = NULL;
	int r = post_asap("mcp.tool_call",
		"{\"name\":\"cron\",\"arguments\":{\"operation\":\"list\"}}",
		"01HZABC124", &code, &body);
	ASSERT(r == 0);
	ASSERT(asap_http_is(code, 200, body));
	ASSERT(body != NULL);
	ASSERT(asap_result_payload_type_is(body, "mcp.tool_result"));
	ASSERT(asap_mcp_result_is_json_array(body));
	free(body);
	return 0;
}

static int test_asap_mcp_unknown_tool(void)
{
	long code;
	char *body = NULL;
	int r = post_asap("mcp.tool_call",
		"{\"name\":\"no-such-tool\",\"arguments\":{}}",
		"01HZABC125", &code, &body);
	ASSERT(r == 0);
	if (code != 400)
		fprintf(stderr, "FAIL: HTTP %ld want 400 body=%s\n",
			code, body ? body : "(null)");
	ASSERT(code == 400);
	ASSERT(body != NULL);
	ASSERT(asap_error_code_is(body, -32001));
	free(body);
	return 0;
}

/* Gateway HTTP buffer is RESP_BUF_SIZE (65536). A (64 KiB - 1) file
 * read wraps into JSON-RPC larger than that; truncation was #61. */
enum { ASAP_OVERSIZE_FILE_BYTES = 65535 };

static int write_asap_oversize_file(char *path, size_t path_sz)
{
	FILE *f;
	char *block;
	size_t nw;
	snprintf(path, path_sz, "%s/.shellclaw/asap_oversize.txt", g_test_home);
	block = malloc(ASAP_OVERSIZE_FILE_BYTES);
	if (!block)
		return -1;
	memset(block, 'A', ASAP_OVERSIZE_FILE_BYTES);
	f = fopen(path, "wb");
	if (!f) {
		free(block);
		return -1;
	}
	nw = fwrite(block, 1, ASAP_OVERSIZE_FILE_BYTES, f);
	fclose(f);
	free(block);
	return nw == ASAP_OVERSIZE_FILE_BYTES ? 0 : -1;
}

static int asap_log_outbound_count(const char *token, int *count_out)
{
	long code;
	char *body = NULL;
	cJSON *root;
	cJSON *ent;
	int i;
	int n;
	int count = 0;
	if (!token || !token[0] || !count_out)
		return -1;
	if (http_get_auth(gw_url("/api/asap/log"), token, &code, &body) != 0)
		return -1;
	if (code != 200 || !body) {
		free(body);
		return -1;
	}
	root = cJSON_Parse(body);
	free(body);
	if (!root)
		return -1;
	ent = cJSON_GetObjectItemCaseSensitive(root, "entries");
	if (!ent || !cJSON_IsArray(ent)) {
		cJSON_Delete(root);
		return -1;
	}
	n = cJSON_GetArraySize(ent);
	for (i = 0; i < n; i++) {
		cJSON *e = cJSON_GetArrayItem(ent, i);
		cJSON *dir = e ? cJSON_GetObjectItemCaseSensitive(e, "direction") : NULL;
		if (dir && cJSON_IsString(dir) && dir->valuestring &&
		    strcmp(dir->valuestring, "out") == 0)
			count++;
	}
	cJSON_Delete(root);
	*count_out = count;
	return 0;
}

static int test_asap_rejects_oversized_response(const char *token)
{
	char path[256];
	char payload[640];
	long code;
	char *body = NULL;
	int n;
	int r;
	int out_before = 0;
	int out_after = 0;
	cJSON *parsed;
	ASSERT(write_asap_oversize_file(path, sizeof path) == 0);
	n = snprintf(payload, sizeof payload,
		"{\"name\":\"file\",\"arguments\":{\"operation\":\"read_file\",\"path\":\"%s\"}}",
		path);
	ASSERT(n > 0 && (size_t)n < sizeof payload);
	if (token && token[0])
		ASSERT(asap_log_outbound_count(token, &out_before) == 0);
	r = post_asap("mcp.tool_call", payload, "01HZABC126", &code, &body);
	ASSERT(r == 0);
	if (code != 500)
		fprintf(stderr, "FAIL: HTTP %ld want 500 body_len=%zu\n",
			code, body ? strlen(body) : 0);
	ASSERT(code == 500);
	ASSERT(body != NULL);
	parsed = cJSON_Parse(body);
	if (!parsed)
		fprintf(stderr, "FAIL: oversized ASAP body is not JSON: %.200s\n",
			body);
	ASSERT(parsed != NULL);
	cJSON_Delete(parsed);
	ASSERT(asap_error_code_is(body, -32603));
	ASSERT(strstr(body, "exceeds gateway buffer") != NULL);
	free(body);
	if (token && token[0]) {
		ASSERT(asap_log_outbound_count(token, &out_after) == 0);
		ASSERT(out_after == out_before);
	}
	return 0;
}

static int test_manifest(void)
{
	long code;
	char *body = NULL;
	int r = http_get(gw_url("/.well-known/asap/manifest.json"), &code, &body);
	ASSERT(r == 0);
	ASSERT(code == 200);
	ASSERT(body != NULL);
	ASSERT(strstr(body, "\"manifest\"") != NULL);
	ASSERT(strstr(body, "\"signature\"") != NULL);
	ASSERT(strstr(body, "\"trust_level\":\"self-signed\"") != NULL ||
	       strstr(body, "\"trust_level\": \"self-signed\"") != NULL);
	ASSERT(strstr(body, "\"public_key\"") != NULL);
	ASSERT(strstr(body, "urn:asap:agent") != NULL);
	ASSERT(strstr(body, "skills") != NULL);
	ASSERT(strstr(body, "endpoints") != NULL);
	free(body);
	return 0;
}

static int test_manifest_rejects_loose_priv(void)
{
	long code;
	char *body = NULL;
	char priv_path[512];
	int r;

	ASSERT(test_manifest() == 0);
	snprintf(priv_path, sizeof(priv_path), "%s/.shellclaw/keys/ed25519.priv",
		 g_test_home);
	ASSERT(chmod(priv_path, 0777) == 0);
	body = NULL;
	r = http_get(gw_url("/.well-known/asap/manifest.json"), &code, &body);
	ASSERT(r == 0);
	ASSERT(code == 500);
	ASSERT(body != NULL);
	ASSERT(strstr(body, "Signing key unavailable") != NULL);
	ASSERT(strstr(body, g_test_home) == NULL);
	ASSERT(strstr(body, "ed25519") == NULL);
	free(body);
	return 0;
}

static int test_asap_body_over_max(void)
{
	long code;
	char *body = NULL;
	const char payload[] = "{}";
	int r = http_post_raw(gw_url("/asap"), payload, sizeof(payload) - 1, "1000001",
			      &code, &body);
	ASSERT(r == 0);
	ASSERT(code == 413);
	if (body)
		free(body);
	return 0;
}

static int test_health_wellknown(void)
{
	long code;
	char *body = NULL;
	int r = http_get(gw_url("/.well-known/asap/health"), &code, &body);
	ASSERT(r == 0);
	ASSERT(code == 200);
	ASSERT(body != NULL);
	ASSERT(strstr(body, "status") != NULL);
	ASSERT(strstr(body, "ok") != NULL);
	free(body);
	return 0;
}

static int test_api_status_401(void)
{
	long code;
	char *body = NULL;
	int r = http_get(gw_url("/api/status"), &code, &body);
	ASSERT(r == 0);
	ASSERT(code == 401);
	free(body);
	return 0;
}

static int test_api_context_snapshot_401(void)
{
	long code;
	char *body = NULL;
	int r = http_get(gw_url("/api/context/snapshot"), &code, &body);
	ASSERT(r == 0);
	ASSERT(code == 401);
	free(body);
	return 0;
}

static int test_api_context_snapshot_get(const char *token)
{
	long code;
	char *body = NULL;
	int r = http_get_auth(gw_url("/api/context/snapshot"), token, &code, &body);
	ASSERT(r == 0);
	ASSERT(code == 200);
	ASSERT(body != NULL);
	ASSERT(strstr(body, "\"dashboard\"") != NULL);
	free(body);
	return 0;
}

static int test_api_status_get(const char *token)
{
	long code;
	char *body = NULL;
	int r = http_get_auth(gw_url("/api/status"), token, &code, &body);
	ASSERT(r == 0);
	ASSERT(code == 200);
	ASSERT(body != NULL);
	ASSERT(strstr(body, "\"active_provider\"") != NULL);
	ASSERT(strstr(body, "\"providers\"") != NULL);
	ASSERT(strstr(body, "\"stub\"") != NULL);
	ASSERT(strstr(body, "\"role\"") != NULL);
	ASSERT(strstr(body, "\"reachable\"") != NULL);
	{
		cJSON *root = cJSON_Parse(body);
		cJSON *ap;
		cJSON *arr;
		cJSON *first;
		cJSON *name_item;
		cJSON *role_item;
		ASSERT(root != NULL);
		ap = cJSON_GetObjectItem(root, "active_provider");
		ASSERT(ap != NULL && cJSON_IsString(ap) && strcmp(ap->valuestring, "stub") == 0);
		arr = cJSON_GetObjectItem(root, "providers");
		ASSERT(arr != NULL && cJSON_IsArray(arr));
		ASSERT(cJSON_GetArraySize(arr) >= 1);
		first = cJSON_GetArrayItem(arr, 0);
		ASSERT(first != NULL);
		name_item = cJSON_GetObjectItem(first, "name");
		ASSERT(name_item != NULL && cJSON_IsString(name_item) &&
		       strcmp(name_item->valuestring, "stub") == 0);
		role_item = cJSON_GetObjectItem(first, "role");
		ASSERT(role_item != NULL && cJSON_IsString(role_item) &&
		       strcmp(role_item->valuestring, "primary") == 0);
		{
			cJSON *discord_item = cJSON_GetObjectItem(root, "discord");
			cJSON *lc;
			ASSERT(discord_item != NULL && cJSON_IsObject(discord_item));
			lc = cJSON_GetObjectItem(discord_item, "lifecycle");
			ASSERT(lc != NULL && cJSON_IsString(lc) &&
			       strcmp(lc->valuestring, "disabled") == 0);
		}
		cJSON_Delete(root);
	}
	free(body);
	return 0;
}

static int test_api_config_get(const char *token)
{
	long code;
	char *body = NULL;
	int r = http_get_auth(gw_url("/api/config"), token, &code, &body);
	ASSERT(r == 0);
	ASSERT(code == 200);
	ASSERT(body != NULL);
	ASSERT(strstr(body, "model") != NULL);
	free(body);
	return 0;
}

static int test_api_config_put_401(void)
{
	long code;
	char *body = NULL;
	const char *valid_toml =
		"[agent]\nmodel = \"blocked\"\n[providers]\nfallback_chain = [ \"stub\" ]\n";
	int r = http_put_auth(gw_url("/api/config"), "invalid-token", valid_toml, &code, &body);
	ASSERT(r == 0);
	ASSERT(code == 401);
	free(body);
	return 0;
}

static int test_api_config_put_invalid_toml(const char *token)
{
	long code;
	char *body = NULL;
	long get_code;
	char *before = NULL;
	int r = http_get_auth(gw_url("/api/config"), token, &get_code, &before);
	ASSERT(r == 0 && get_code == 200 && before != NULL);
	r = http_put_auth(gw_url("/api/config"), token, "[[[not valid toml", &code, &body);
	ASSERT(r == 0);
	ASSERT(code == 400);
	ASSERT(body != NULL);
	free(body);
	body = NULL;
	r = http_get_auth(gw_url("/api/config"), token, &get_code, &body);
	ASSERT(r == 0 && get_code == 200);
	ASSERT(body != NULL);
	ASSERT(strcmp(body, before) == 0);
	free(before);
	free(body);
	return 0;
}

static int test_api_config_put_valid(const char *token, int port, const char *config_path)
{
	long code;
	char *body = NULL;
	char put_toml[512];
	char disk_buf[4096];
	FILE *disk_fp;
	snprintf(put_toml, sizeof(put_toml),
		 "[agent]\nmodel = \"integration_updated\"\n"
		 "[providers]\nfallback_chain = [ \"stub\" ]\n"
		 "[gateway]\nenabled = true\nhost = \"127.0.0.1\"\nport = %d\n"
		 "[memory]\ndb_path = \"%s/.shellclaw/memory.db\"\n"
		 "[skills]\ndir = \"%s/.shellclaw/skills\"\n",
		 port, g_test_home, g_test_home);
	int r = http_put_auth(gw_url("/api/config"), token, put_toml, &code, &body);
	ASSERT(r == 0);
	ASSERT(code == 200);
	ASSERT(body != NULL);
	ASSERT(strstr(body, "\"ok\"") != NULL);
	free(body);
	disk_fp = fopen(config_path, "r");
	ASSERT(disk_fp != NULL);
	ASSERT(fread(disk_buf, 1, sizeof(disk_buf) - 1, disk_fp) > 0);
	disk_buf[sizeof(disk_buf) - 1] = '\0';
	fclose(disk_fp);
	ASSERT(strstr(disk_buf, "integration_updated") != NULL);
	return 0;
}

static int test_api_skills_list(const char *token)
{
	long code;
	char *body = NULL;
	int r = http_get_auth(gw_url("/api/skills"), token, &code, &body);
	ASSERT(r == 0);
	ASSERT(code == 200);
	ASSERT(body != NULL);
	ASSERT(strstr(body, "[") != NULL);
	free(body);
	return 0;
}

static int test_api_skill_create_delete(const char *token)
{
	long code;
	char *body = NULL;
	int r = http_post_auth(gw_url("/api/skills"), token,
		"{\"name\":\"test_integration_skill\",\"content\":\"# Test skill for integration\"}",
		&code, &body);
	ASSERT(r == 0);
	ASSERT(code == 200 || code == 201);
	free(body);
	body = NULL;
	r = http_delete_auth(gw_url("/api/skills/test_integration_skill"), token, &code, &body);
	ASSERT(r == 0);
	ASSERT(code == 200);
	free(body);
	return 0;
}

static int test_api_memory(const char *token)
{
	long code;
	char *body = NULL;
	int r = http_get_auth(gw_url("/api/memory?q=test&limit=5"), token, &code, &body);
	ASSERT(r == 0);
	ASSERT(code == 200);
	ASSERT(body != NULL);
	free(body);
	return 0;
}

static int test_api_cron_list(const char *token)
{
	long code;
	char *body = NULL;
	int r = http_get_auth(gw_url("/api/cron"), token, &code, &body);
	ASSERT(r == 0);
	ASSERT(code == 200);
	ASSERT(body != NULL);
	ASSERT(strstr(body, "[") != NULL);
	free(body);
	return 0;
}

static int test_api_cron_create_delete(const char *token)
{
	long code;
	char *body = NULL;
	int r = http_post_auth(gw_url("/api/cron"), token,
		"{\"schedule\":\"interval:3600\",\"message\":\"integration test\",\"channel\":\"cli\",\"recipient\":\"default\"}",
		&code, &body);
	ASSERT(r == 0);
	ASSERT(code == 200 || code == 201);
	ASSERT(body != NULL);
	cJSON *root = cJSON_Parse(body);
	ASSERT(root != NULL);
	cJSON *id_obj = cJSON_GetObjectItem(root, "id");
	ASSERT(id_obj != NULL && cJSON_IsString(id_obj));
	char del_url[256];
	snprintf(del_url, sizeof(del_url), "%s/api/cron/%s", g_base_url, id_obj->valuestring);
	cJSON_Delete(root);
	free(body);
	body = NULL;
	r = http_delete_auth(del_url, token, &code, &body);
	ASSERT(r == 0);
	ASSERT(code == 200);
	free(body);
	return 0;
}

static int test_api_cron_toggle_post(const char *token)
{
	long code;
	char *body = NULL;
	int r = http_post_auth(gw_url("/api/cron"), token,
		"{\"schedule\":\"interval:3600\",\"message\":\"toggle test\",\"channel\":\"cli\",\"recipient\":\"default\"}",
		&code, &body);
	ASSERT(r == 0);
	ASSERT(code == 200 || code == 201);
	ASSERT(body != NULL);
	cJSON *root = cJSON_Parse(body);
	ASSERT(root != NULL);
	cJSON *id_obj = cJSON_GetObjectItem(root, "id");
	ASSERT(id_obj != NULL && cJSON_IsString(id_obj));
	char id[128];
	snprintf(id, sizeof(id), "%s", id_obj->valuestring);
	cJSON_Delete(root);
	free(body);
	body = NULL;

	/* Regression gate: before the routes.c length-guard fix, POST /api/cron/<id>/toggle
	 * fell through to the DELETE-only branch and returned 405. After the fix it must
	 * return 200. */
	char toggle_url[512];
	snprintf(toggle_url, sizeof(toggle_url), "%s/api/cron/%s/toggle", g_base_url, id);
	r = http_post_auth(toggle_url, token, "", &code, &body);
	ASSERT(r == 0);
	ASSERT(code == 200);
	ASSERT(body != NULL);
	ASSERT(strstr(body, "\"ok\"") != NULL);
	free(body);
	body = NULL;

	/* Trailing garbage must NOT be treated as a toggle (exact-match guard). */
	char junk_url[512];
	snprintf(junk_url, sizeof(junk_url), "%s/api/cron/%s/toggle/extra", g_base_url, id);
	r = http_post_auth(junk_url, token, "", &code, &body);
	ASSERT(r == 0);
	ASSERT(code == 405);
	free(body);
	body = NULL;

	char del_url[512];
	snprintf(del_url, sizeof(del_url), "%s/api/cron/%s", g_base_url, id);
	r = http_delete_auth(del_url, token, &code, &body);
	ASSERT(r == 0);
	ASSERT(code == 200);
	free(body);
	return 0;
}

static int test_api_sessions(const char *token)
{
	long code;
	char *body = NULL;
	int r = http_get_auth(gw_url("/api/sessions"), token, &code, &body);
	ASSERT(r == 0);
	ASSERT(code == 200);
	ASSERT(body != NULL);
	free(body);
	return 0;
}

static int test_api_asap_log_401(void)
{
	long code;
	char *body = NULL;
	int r = http_get(gw_url("/api/asap/log"), &code, &body);
	ASSERT(r == 0);
	ASSERT(code == 401);
	free(body);
	return 0;
}

#define SHUTDOWN_LOAD_THREADS 4
#define SHUTDOWN_LOAD_START_SPINS 50

/* Bearer GETs that stay in-flight across SIGTERM so auth_validate_token
 * still runs while cleanup_subsystems joins the lws thread. */
struct shutdown_load {
	char url[256];
	const char *token;
	volatile sig_atomic_t stop;
	volatile sig_atomic_t started;
};

static void *shutdown_load_thread(void *arg)
{
	struct shutdown_load *load = (struct shutdown_load *)arg;
	while (!load->stop) {
		long code = 0;
		char *body = NULL;
		load->started = 1;
		(void)http_get_auth(load->url, load->token, &code, &body);
		free(body);
	}
	return NULL;
}

static int shutdown_load_spawn(struct shutdown_load *load, pthread_t *thds, int n)
{
	int i;
	int ncreated = 0;
	for (i = 0; i < n; i++) {
		if (pthread_create(&thds[ncreated], NULL, shutdown_load_thread, load) != 0)
			continue;
		ncreated++;
	}
	return ncreated;
}

static void shutdown_load_wait_started(const struct shutdown_load *load)
{
	int i;
	for (i = 0; i < SHUTDOWN_LOAD_START_SPINS; i++) {
		struct timespec delay = { 0, 10000000L };
		if (load->started)
			return;
		(void)nanosleep(&delay, NULL);
	}
}

static void shutdown_load_join(struct shutdown_load *load, pthread_t *thds, int ncreated)
{
	int i;
	load->stop = 1;
	for (i = 0; i < ncreated; i++)
		(void)pthread_join(thds[i], NULL);
}

static int shutdown_crash_status(int status)
{
	int sig;
	if (!WIFSIGNALED(status))
		return 0;
	sig = WTERMSIG(status);
	if (sig == SIGSEGV || sig == SIGABRT || sig == SIGBUS || sig == SIGILL) {
		fprintf(stderr, "FAIL: gateway crashed on shutdown with signal %d\n", sig);
		return 1;
	}
	return 0;
}

static int test_shutdown_does_not_crash(pid_t pid, const char *token, int *reaped)
{
	struct shutdown_load load;
	pthread_t thds[SHUTDOWN_LOAD_THREADS];
	int ncreated = 0;
	int status = 0;
	if (reaped)
		*reaped = 0;
	memset(&load, 0, sizeof(load));
	if (token && token[0]) {
		(void)curl_global_init(CURL_GLOBAL_DEFAULT);
		load.token = token;
		snprintf(load.url, sizeof(load.url), "%s/api/status", g_base_url);
		ncreated = shutdown_load_spawn(&load, thds, SHUTDOWN_LOAD_THREADS);
		shutdown_load_wait_started(&load);
	}
	if (kill(pid, SIGTERM) != 0) {
		shutdown_load_join(&load, thds, ncreated);
		return 1;
	}
	if (waitpid(pid, &status, 0) != pid) {
		shutdown_load_join(&load, thds, ncreated);
		return 1;
	}
	if (reaped)
		*reaped = 1;
	shutdown_load_join(&load, thds, ncreated);
	return shutdown_crash_status(status);
}

static int test_api_asap_log(const char *token)
{
	long code;
	char *body = NULL;
	int r = http_get_auth(gw_url("/api/asap/log"), token, &code, &body);
	ASSERT(r == 0);
	ASSERT(code == 200);
	ASSERT(body != NULL);
	ASSERT(strstr(body, "\"entries\"") != NULL);
	cJSON *root = cJSON_Parse(body);
	ASSERT(root != NULL);
	cJSON *ent = cJSON_GetObjectItem(root, "entries");
	ASSERT(ent != NULL && cJSON_IsArray(ent));
	cJSON_Delete(root);
	free(body);
	return 0;
}

static int test_api_hardware_board_401(void)
{
	long code;
	char *body = NULL;
	int r = http_get(gw_url("/api/hardware/board"), &code, &body);
	ASSERT(r == 0);
	ASSERT(code == 401);
	free(body);
	return 0;
}

static int test_api_hardware_gpio_401(void)
{
	long code;
	char *body = NULL;
	int r = http_get(gw_url("/api/hardware/gpio"), &code, &body);
	ASSERT(r == 0);
	ASSERT(code == 401);
	free(body);
	return 0;
}

static int test_api_hardware_board_get(const char *token)
{
	long code;
	char *body = NULL;
	int r = http_get_auth(gw_url("/api/hardware/board"), token, &code, &body);
	ASSERT(r == 0);
	ASSERT(code == 200);
	ASSERT(body != NULL);
	{
		cJSON *root = cJSON_Parse(body);
		cJSON *id;
		cJSON *backends;
		ASSERT(root != NULL);
		id = cJSON_GetObjectItem(root, "id");
		backends = cJSON_GetObjectItem(root, "backends");
		ASSERT(id != NULL && cJSON_IsString(id));
		ASSERT(backends != NULL && cJSON_IsObject(backends));
		cJSON_Delete(root);
	}
	free(body);
	return 0;
}

static int test_api_hardware_gpio_get(const char *token)
{
	long code;
	char *body = NULL;
	int r = http_get_auth(gw_url("/api/hardware/gpio"), token, &code, &body);
	ASSERT(r == 0);
	ASSERT(code == 200 || code == 503);
	free(body);
	return 0;
}

static int test_api_hardware_sensors_deferred(const char *token)
{
	long code;
	char *body = NULL;
	int r = http_get_auth(gw_url("/api/hardware/sensors"), token, &code, &body);
	ASSERT(r == 0);
	ASSERT(code == 200);
	ASSERT(body != NULL);
	{
		cJSON *root = cJSON_Parse(body);
		cJSON *st;
		cJSON *msg;
		ASSERT(root != NULL);
		st = cJSON_GetObjectItem(root, "status");
		msg = cJSON_GetObjectItem(root, "message");
		ASSERT(st != NULL && cJSON_IsString(st) &&
		       strcmp(st->valuestring, "deferred_v12") == 0);
		ASSERT(msg != NULL && cJSON_IsString(msg) &&
		       strcmp(msg->valuestring,
			      "sensor decoders ship in v1.2 (Phase 7)") == 0);
		cJSON_Delete(root);
	}
	free(body);
	return 0;
}

static int test_api_hardware_camera_snapshot_401(void)
{
	long code;
	char *body = NULL;
	int r = http_post(gw_url("/api/hardware/camera/snapshot"), "{}", &code, &body);
	ASSERT(r == 0);
	ASSERT(code == 401);
	free(body);
	return 0;
}

static int test_api_hardware_camera_deferred(const char *token)
{
	long code;
	char *body = NULL;
	int r = http_post_auth(gw_url("/api/hardware/camera/snapshot"), token, "{}", &code,
			       &body);
	ASSERT(r == 0);
	ASSERT(code == 200);
	ASSERT(body != NULL);
	{
		cJSON *root = cJSON_Parse(body);
		cJSON *st;
		cJSON *msg;
		ASSERT(root != NULL);
		st = cJSON_GetObjectItem(root, "status");
		msg = cJSON_GetObjectItem(root, "message");
		ASSERT(st != NULL && cJSON_IsString(st) &&
		       strcmp(st->valuestring, "deferred_v12") == 0);
		ASSERT(msg != NULL && cJSON_IsString(msg) &&
		       strcmp(msg->valuestring,
			      "camera image return path ships in v1.2 (Phase 7)") == 0);
		cJSON_Delete(root);
	}
	free(body);
	return 0;
}

int main(int argc, char **argv)
{
	(void)argc;
	(void)argv;
#ifndef SHELLCLAW_GATEWAY
	fprintf(stderr, "test_gateway_http: skipped (gateway not built)\n");
	return 0;
#else
	char config_path[256];
	char skills_dir[256];
	char db_path[256];
	char shellclaw_dir[128];
	char tokens_path[160];
	char pairing_file[160];
	int port;
	snprintf(g_test_home, sizeof(g_test_home), "/tmp/shellclaw_test_gw_%d", (int)getpid());
	snprintf(config_path, sizeof(config_path), "%s/config.toml", g_test_home);
	snprintf(shellclaw_dir, sizeof(shellclaw_dir), "%s/.shellclaw", g_test_home);
	snprintf(tokens_path, sizeof(tokens_path), "%s/.shellclaw/auth_tokens.json", g_test_home);
	snprintf(pairing_file, sizeof(pairing_file), "%s/.shellclaw/test_pairing_code", g_test_home);
	snprintf(skills_dir, sizeof(skills_dir), "%s/.shellclaw/skills", g_test_home);
	snprintf(db_path, sizeof(db_path), "%s/.shellclaw/memory.db", g_test_home);
	port = pick_ephemeral_port();
	if (port <= 0) {
		fprintf(stderr, "test_gateway_http: ephemeral port failed\n");
		return 1;
	}
	snprintf(g_base_url, sizeof(g_base_url), "http://127.0.0.1:%d", port);
	if (mkdir(g_test_home, 0755) != 0 && errno != EEXIST) {
		fprintf(stderr, "test_gateway_http: mkdir failed\n");
		return 1;
	}
	if (mkdir(shellclaw_dir, 0755) != 0 && errno != EEXIST) {
		fprintf(stderr, "test_gateway_http: mkdir .shellclaw failed\n");
		return 1;
	}
	if (mkdir(skills_dir, 0755) != 0 && errno != EEXIST) {
		fprintf(stderr, "test_gateway_http: mkdir skills failed\n");
		return 1;
	}
	unlink(tokens_path);
	unlink(pairing_file);
	FILE *f = fopen(config_path, "w");
	if (!f) {
		fprintf(stderr, "test_gateway_http: cannot write config\n");
		return 1;
	}
	fprintf(f, "[agent]\nmodel = \"test\"\n");
	fprintf(f, "[providers]\nfallback_chain = [ \"stub\" ]\n");
	fprintf(f, "[gateway]\nenabled = true\nhost = \"127.0.0.1\"\nport = %d\n", port);
	fprintf(f, "[memory]\ndb_path = \"%s/.shellclaw/memory.db\"\n", g_test_home);
	fprintf(f, "[skills]\ndir = \"%s\"\n", skills_dir);
	fclose(f);
	setenv("HOME", g_test_home, 1);
	setenv("SHELLCLAW_TEST_MODE", "1", 1);
	pid_t pid = fork();
	if (pid < 0) {
		fprintf(stderr, "test_gateway_http: fork failed\n");
		return 1;
	}
	if (pid == 0) {
		int devnull = open("/dev/null", O_WRONLY);
		if (devnull >= 0) {
			dup2(devnull, STDOUT_FILENO);
			dup2(devnull, STDERR_FILENO);
			close(devnull);
		}
		execl("./build/shellclaw", "shellclaw", "--config", config_path, (char *)NULL);
		_exit(1);
	}
	char pairing_code[16] = {0};
	if (wait_for_health(40) != 0) {
		fprintf(stderr, "test_gateway_http: /health poll timeout on %s\n", g_base_url);
		kill(pid, SIGTERM);
		waitpid(pid, NULL, 0);
		return 1;
	}
	if (read_pairing_code_from_file(g_test_home, pairing_code, sizeof(pairing_code)) != 0) {
		fprintf(stderr, "test_gateway_http: failed to read pairing code file\n");
		kill(pid, SIGTERM);
		waitpid(pid, NULL, 0);
		return 1;
	}
	char token[128] = {0};
	int failed = 0;
	int shutdown_reaped = 0;
	if (test_health() != 0) { fprintf(stderr, "test_health failed\n"); failed++; }
	if (test_pair(pairing_code, token, sizeof(token)) != 0) {
		fprintf(stderr, "test_pair failed\n");
		failed++;
	}
	if (test_api_config_401() != 0) { fprintf(stderr, "test_api_config_401 failed\n"); failed++; }
	if (test_api_config_401_malformed_auth() != 0) {
		fprintf(stderr, "test_api_config_401_malformed_auth failed\n");
		failed++;
	}
	if (test_api_config_put_401() != 0) { fprintf(stderr, "test_api_config_put_401 failed\n"); failed++; }
	if (test_api_status_401() != 0) { fprintf(stderr, "test_api_status_401 failed\n"); failed++; }
	if (test_api_context_snapshot_401() != 0) { fprintf(stderr, "test_api_context_snapshot_401 failed\n"); failed++; }
	if (test_manifest() != 0) { fprintf(stderr, "test_manifest failed\n"); failed++; }
	if (test_manifest_rejects_loose_priv() != 0) {
		fprintf(stderr, "test_manifest_rejects_loose_priv failed\n");
		failed++;
	}
	if (test_health_wellknown() != 0) { fprintf(stderr, "test_health_wellknown failed\n"); failed++; }
	if (test_asap_body_over_max() != 0) {
		fprintf(stderr, "test_asap_body_over_max failed\n");
		failed++;
	}
	if (test_asap_invalid_body() != 0) { fprintf(stderr, "test_asap_invalid_body failed\n"); failed++; }
	if (test_asap_missing_fields() != 0) { fprintf(stderr, "test_asap_missing_fields failed\n"); failed++; }
	if (test_asap_task_request() != 0) { fprintf(stderr, "test_asap_task_request failed\n"); failed++; }
	if (test_asap_mcp_tool_call() != 0) { fprintf(stderr, "test_asap_mcp_tool_call failed\n"); failed++; }
	if (test_asap_mcp_unknown_tool() != 0) { fprintf(stderr, "test_asap_mcp_unknown_tool failed\n"); failed++; }
	if (test_asap_rejects_oversized_response(token) != 0) {
		fprintf(stderr, "test_asap_rejects_oversized_response failed\n");
		failed++;
	}
	if (test_api_asap_log_401() != 0) { fprintf(stderr, "test_api_asap_log_401 failed\n"); failed++; }
	if (test_api_hardware_board_401() != 0) {
		fprintf(stderr, "test_api_hardware_board_401 failed\n");
		failed++;
	}
	if (test_api_hardware_gpio_401() != 0) {
		fprintf(stderr, "test_api_hardware_gpio_401 failed\n");
		failed++;
	}
	if (test_api_hardware_camera_snapshot_401() != 0) {
		fprintf(stderr, "test_api_hardware_camera_snapshot_401 failed\n");
		failed++;
	}
	if (token[0]) {
		if (test_api_config_invalid_bearer(token) != 0) {
			fprintf(stderr, "test_api_config_invalid_bearer failed\n");
			failed++;
		}
		if (test_api_config_get(token) != 0) { fprintf(stderr, "test_api_config_get failed\n"); failed++; }
		if (test_api_config_put_invalid_toml(token) != 0) {
			fprintf(stderr, "test_api_config_put_invalid_toml failed\n");
			failed++;
		}
		if (test_api_config_put_valid(token, port, config_path) != 0) {
			fprintf(stderr, "test_api_config_put_valid failed\n");
			failed++;
		}
		if (test_api_status_get(token) != 0) { fprintf(stderr, "test_api_status_get failed\n"); failed++; }
		if (test_api_context_snapshot_get(token) != 0) { fprintf(stderr, "test_api_context_snapshot_get failed\n"); failed++; }
		if (test_api_skills_list(token) != 0) { fprintf(stderr, "test_api_skills_list failed\n"); failed++; }
		if (test_api_skill_create_delete(token) != 0) { fprintf(stderr, "test_api_skill_create_delete failed\n"); failed++; }
		if (test_api_memory(token) != 0) { fprintf(stderr, "test_api_memory failed\n"); failed++; }
		if (test_api_cron_list(token) != 0) { fprintf(stderr, "test_api_cron_list failed\n"); failed++; }
		if (test_api_cron_create_delete(token) != 0) { fprintf(stderr, "test_api_cron_create_delete failed\n"); failed++; }
		if (test_api_cron_toggle_post(token) != 0) { fprintf(stderr, "test_api_cron_toggle_post failed\n"); failed++; }
		if (test_api_sessions(token) != 0) { fprintf(stderr, "test_api_sessions failed\n"); failed++; }
		if (test_api_asap_log(token) != 0) { fprintf(stderr, "test_api_asap_log failed\n"); failed++; }
		if (test_api_hardware_board_get(token) != 0) {
			fprintf(stderr, "test_api_hardware_board_get failed\n");
			failed++;
		}
		if (test_api_hardware_gpio_get(token) != 0) {
			fprintf(stderr, "test_api_hardware_gpio_get failed\n");
			failed++;
		}
		if (test_api_hardware_sensors_deferred(token) != 0) {
			fprintf(stderr, "test_api_hardware_sensors_deferred failed\n");
			failed++;
		}
		if (test_api_hardware_camera_deferred(token) != 0) {
			fprintf(stderr, "test_api_hardware_camera_deferred failed\n");
			failed++;
		}
	}
	if (test_shutdown_does_not_crash(pid, token, &shutdown_reaped) != 0) {
		fprintf(stderr, "test_shutdown_does_not_crash failed\n");
		failed++;
		if (!shutdown_reaped) {
			kill(pid, SIGKILL);
			waitpid(pid, NULL, 0);
		}
	}
	unlink(config_path);
	unlink(tokens_path);
	unlink(pairing_file);
	unlink(db_path);
	if (failed == 0)
		printf("test_gateway_http: all tests passed\n");
	return failed;
#endif
}
