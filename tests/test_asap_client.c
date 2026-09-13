/**
 * @file test_asap_client.c
 * @brief Tests for JSON-RPC request/response helpers and ASAP client error paths.
 */
#define _POSIX_C_SOURCE 200809L

#include "asap/client.h"
#include "asap/envelope.h"
#include "core/config.h"
#include "cJSON.h"
#include <curl/curl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <sys/socket.h>


struct tiny_http_srv {
	pthread_t thd;
	long http_code;
	const char *entity_body;
	size_t entity_len;
	volatile unsigned short bind_port;
	volatile int started;
	volatile int join_err;
	int listen_fd;
};

static ssize_t recv_until_headers_complete(int fd, char *buf, size_t cap)
{
	size_t total = 0;
	while (total + 2 < cap) {
		ssize_t n = recv(fd, buf + total, 1, 0);
		if (n <= 0)
			return -1;
		total += (size_t)n;
		buf[total] = '\0';
		if (strstr(buf, "\r\n\r\n") != NULL)
			return (ssize_t)total;
	}
	return -1;
}

static void *tiny_http_thread_main(void *v)
{
	struct tiny_http_srv *s = v;
	struct sockaddr_in sa;
	struct sockaddr_in peer;
	int cli;
	ssize_t n;
	size_t hdr_cap = 6144;
	char req_hdr_stk[6144];

	s->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
	if (s->listen_fd < 0) {
		s->join_err = 1;
		return NULL;
	}
	(void)setsockopt(s->listen_fd, SOL_SOCKET, SO_REUSEADDR, &(int){1}, (socklen_t)sizeof(int));
	memset(&sa, 0, sizeof sa);
	sa.sin_family = AF_INET;
	sa.sin_addr.s_addr = htonl(0x7f000001U);
	sa.sin_port = htons(0);
	if (bind(s->listen_fd, (struct sockaddr *)&sa, (socklen_t)sizeof sa) != 0) {
		close(s->listen_fd);
		s->listen_fd = -1;
		s->join_err = 1;
		return NULL;
	}
	{
		socklen_t sl = (socklen_t)sizeof sa;
		if (getsockname(s->listen_fd, (struct sockaddr *)&sa, &sl) != 0) {
			close(s->listen_fd);
			s->listen_fd = -1;
			s->join_err = 1;
			return NULL;
		}
		s->bind_port = ntohs(sa.sin_port);
	}
	if (listen(s->listen_fd, 1) != 0) {
		close(s->listen_fd);
		s->listen_fd = -1;
		s->join_err = 1;
		return NULL;
	}
	s->started = 1;
	memset(&peer, 0, sizeof peer);
	{
		socklen_t pl = (socklen_t)sizeof peer;
		cli = accept(s->listen_fd, (struct sockaddr *)&peer, &pl);
	}
	if (cli < 0) {
		s->join_err = 1;
		close(s->listen_fd);
		s->listen_fd = -1;
		return NULL;
	}
	n = recv_until_headers_complete(cli, req_hdr_stk, hdr_cap);
	(void)n;
	{
		char hdr[384];
		int hl = snprintf(hdr, sizeof hdr,
				  "HTTP/1.1 %ld xxx\r\n"
				  "Content-Type: application/json\r\n"
				  "Content-Length: %zu\r\n"
				  "Connection: close\r\n"
				  "\r\n",
				  s->http_code,
				  s->entity_len ? s->entity_len : (size_t)0);
		if (hl <= 0 || (size_t)hl >= sizeof hdr) {
			s->join_err = 1;
			close(cli);
			close(s->listen_fd);
			s->listen_fd = -1;
			return NULL;
		}
		(void)!write(cli, hdr, (size_t)hl);
		if (s->entity_len > 0 && s->entity_body != NULL)
			(void)!write(cli, s->entity_body, s->entity_len);
	}
	shutdown(cli, SHUT_RDWR);
	close(cli);
	close(s->listen_fd);
	s->listen_fd = -1;
	return NULL;
}

static int tiny_http_start(struct tiny_http_srv *s, long http_code,
			   const char *entity_body, size_t entity_len)
{
	memset(s, 0, sizeof *s);
	s->http_code = http_code;
	s->entity_body = entity_body;
	s->entity_len = entity_len;
	s->listen_fd = -1;
	if (pthread_create(&s->thd, NULL, tiny_http_thread_main, s) != 0)
		return -1;
	for (int spins = 0; spins < 500 && !s->started; spins++) {
		struct timespec ts = {.tv_sec = 0, .tv_nsec = 2000000L};
		(void)nanosleep(&ts, NULL);
	}
	if (!s->started || s->bind_port == 0 || s->join_err)
		return -1;
	return 0;
}

static void tiny_http_join(struct tiny_http_srv *s)
{
	(void)pthread_join(s->thd, NULL);
	if (s->listen_fd >= 0)
		close(s->listen_fd);
}

#define ASSERT(c) do { if (!(c)) { fprintf(stderr, "FAIL: %s:%d %s\n", __FILE__, __LINE__, #c); return 1; } } while (0)

static int fill_min_task_request(asap_envelope_t *e)
{
	cJSON *pl = cJSON_CreateObject();
	if (!pl) return -1;
	if (!cJSON_AddStringToObject(pl, "input", "test")) { cJSON_Delete(pl); return -1; }
	asap_envelope_init(e);
	e->id = strdup("01HZTEST00");
	e->asap_version = strdup("2.1");
	e->sender = strdup("urn:asap:agent:from");
	e->recipient = strdup("urn:asap:agent:to");
	e->payload_type = strdup("task.request");
	e->payload = pl;
	return 0;
}

static int test_request_roundtrip_string(void)
{
	asap_envelope_t env;
	cJSON *id = cJSON_CreateString("rid-1");
	char *s;
	cJSON *root;
	cJSON *params;
	cJSON *rid;
	int ok;

	ASSERT(fill_min_task_request(&env) == 0);
	cJSON *dup_id = cJSON_Duplicate(id, 1);
	cJSON_Delete(id);
	s = asap_envelope_to_jsonrpc_request_string(&env, dup_id, "asap.send");
	cJSON_Delete(dup_id);
	ASSERT(s != NULL);
	ASSERT(strstr(s, "\"asap.send\"") != NULL);
	ASSERT(strstr(s, "task.request") != NULL);
	root = cJSON_Parse(s);
	free(s);
	ASSERT(root != NULL);
	rid = cJSON_GetObjectItemCaseSensitive(root, "id");
	ASSERT(rid && cJSON_IsString(rid) && strcmp(rid->valuestring, "rid-1") == 0);
	params = cJSON_GetObjectItemCaseSensitive(root, "params");
	ASSERT(params);
	ok = cJSON_GetObjectItemCaseSensitive(params, "id") && cJSON_GetObjectItemCaseSensitive(params, "payload");
	cJSON_Delete(root);
	ASSERT(ok);
	asap_envelope_clear(&env);
	return 0;
}

static int test_parse_response_ok(void)
{
	const char *json = "{\"jsonrpc\":\"2.0\","
		"\"result\":{"
		"\"id\":\"t1\","
		"\"asap_version\":\"2.1\","
		"\"sender\":\"a\",\"recipient\":\"b\","
		"\"payload_type\":\"task.response\","
		"\"payload\":{\"out\":\"x\"}"
		"},"
		"\"id\":\"rid\"}";
	char err[128];
	asap_envelope_t out;
	asap_envelope_init(&out);
	err[0] = 0;
	ASSERT(asap_envelope_parse_jsonrpc_response(json, &out, err, sizeof err) == 0);
	ASSERT(strcmp(err, "") == 0);
	ASSERT(out.jsonrpc_request_id != NULL);
	ASSERT(strcmp(out.id, "t1") == 0);
	ASSERT(strcmp(out.payload_type, "task.response") == 0);
	ASSERT(out.payload != NULL);
	ASSERT(cJSON_GetObjectItem(out.payload, "out") != NULL);
	asap_envelope_clear(&out);
	return 0;
}

static int test_parse_response_jsonrpc_error(void)
{
	const char *json = "{\"jsonrpc\":\"2.0\","
		"\"error\":{\"code\":-32600,\"message\":\"nope\"},"
		"\"id\":1}";
	char err[128];
	asap_envelope_t out;
	asap_envelope_init(&out);
	err[0] = 0;
	ASSERT(asap_envelope_parse_jsonrpc_response(json, &out, err, sizeof err) == -1);
	ASSERT(strstr(err, "nope") != NULL);
	asap_envelope_clear(&out);
	return 0;
}

/* HTTP 200 result missing required envelope fields used to double-free rpc_id (Refs: #84). */
static const char jsonrpc_result_missing_payload[] =
	"{\"jsonrpc\":\"2.0\","
	"\"id\":\"1\","
	"\"result\":{"
	"\"id\":\"t1\","
	"\"asap_version\":\"2.1\","
	"\"sender\":\"a\",\"recipient\":\"b\","
	"\"payload_type\":\"task.response\""
	"}}";

static int test_parse_response_missing_payload(void)
{
	char err[128];
	asap_envelope_t out;
	asap_envelope_init(&out);
	err[0] = 0;
	ASSERT(asap_envelope_parse_jsonrpc_response(jsonrpc_result_missing_payload, &out, err, sizeof err) == -1);
	ASSERT(strstr(err, "payload") != NULL);
	asap_envelope_clear(&out);
	return 0;
}

static int test_config_defaults(void)
{
	asap_client_config_t c;
	asap_client_config_init(&c);
	ASSERT(c.timeout_sec == 30L);
	ASSERT(c.ssl_verifypeer == 1);
	return 0;
}

static int test_send_fails_no_server(void)
{
	asap_envelope_t env, resp;
	char err[256];
	ASSERT(fill_min_task_request(&env) == 0);
	asap_envelope_init(&resp);
	ASSERT(asap_client_send_task("http://127.0.0.1:1/", NULL, "asap.send", &env, NULL, NULL, &resp, err, sizeof err) == -1);
	ASSERT(err[0] != '\0');
	asap_envelope_clear(&env);
	asap_envelope_clear(&resp);
	return 0;
}

static const char live_ok_body[] =
	"{\"jsonrpc\":\"2.0\","
	"\"result\":{"
	"\"id\":\"t1\","
	"\"asap_version\":\"2.1\","
	"\"sender\":\"a\",\"recipient\":\"b\","
	"\"payload_type\":\"task.response\","
	"\"payload\":{\"out\":\"live-line\"}"
	"},"
	"\"id\":null}";

static const char live_ok_body_matching_req[] =
	"{\"jsonrpc\":\"2.0\","
	"\"result\":{"
	"\"id\":\"t2\","
	"\"asap_version\":\"2.1\","
	"\"sender\":\"a\",\"recipient\":\"b\","
	"\"payload_type\":\"task.response\","
	"\"payload\":{\"out\":\"id-match-line\"}"
	"},"
	"\"id\":\"fixed-req-tag\"}";

static int test_config_from_config_timeout(void)
{
	const char *path = "/tmp/shellclaw_asap_cli_timeout.toml";
	FILE *fp;
	config_t *cfg;
	asap_client_config_t cli;
	fp = fopen(path, "w");
	ASSERT(fp != NULL);
	fprintf(fp, "[agent]\nmodel = \"x\"\n");
	fprintf(fp, "[asap]\nclient_timeout_sec = 71\n");
	fclose(fp);
	ASSERT(config_load(path, &cfg, NULL, 0) == 0);
	asap_client_config_from_config(cfg, &cli);
	ASSERT(cli.timeout_sec == 71L);
	ASSERT(cli.connect_timeout_sec == 30L);
	config_free(cfg);
	(void)!remove(path);
	return 0;
}

static int test_send_invalid_arguments(void)
{
	asap_envelope_t env;
	asap_envelope_t resp;
	char err[256];
	ASSERT(fill_min_task_request(&env) == 0);
	asap_envelope_init(&resp);
	err[0] = '\0';
	ASSERT(asap_client_send_task(NULL, NULL, ASAP_DEFAULT_JSONRPC_METHOD, &env, NULL, NULL, &resp,
				     err, sizeof err) == -1);
	ASSERT(err[0] != '\0');
	err[0] = '\0';
	ASSERT(asap_client_send_task("http://127.0.0.1:9/", NULL, ASAP_DEFAULT_JSONRPC_METHOD, NULL, NULL,
				     NULL, &resp, err, sizeof err) == -1);
	ASSERT(err[0] != '\0');
	err[0] = '\0';
	ASSERT(asap_client_send_task("http://127.0.0.1:9/", NULL, ASAP_DEFAULT_JSONRPC_METHOD, &env, NULL,
				     NULL, NULL, err, sizeof err) == -1);
	asap_envelope_clear(&env);
	asap_envelope_clear(&resp);
	return 0;
}

static int test_live_http_roundtrip_success(void)
{
	asap_envelope_t env;
	asap_envelope_t resp;
	struct tiny_http_srv srv;
	char err[256];
	char url[96];
	asap_client_config_t client_cfg;

	ASSERT(fill_min_task_request(&env) == 0);
	asap_client_config_init(&client_cfg);
	client_cfg.ssl_verifypeer = 0;
	client_cfg.timeout_sec = 5L;
	client_cfg.connect_timeout_sec = 5L;
	ASSERT(tiny_http_start(&srv, 200L, live_ok_body, strlen(live_ok_body)) == 0);
	asap_envelope_init(&resp);
	err[0] = '\0';
	ASSERT(snprintf(url, sizeof url, "http://127.0.0.1:%hu/", srv.bind_port) < (int)sizeof url);
	ASSERT(asap_client_send_task(url, "test-bearer-token", ASAP_DEFAULT_JSONRPC_METHOD, &env, &client_cfg,
				     NULL, &resp, err, sizeof err) == 0);
	ASSERT(err[0] == '\0');
	ASSERT(resp.payload != NULL);
	ASSERT(strcmp(resp.payload_type, "task.response") == 0);
	ASSERT(cJSON_GetObjectItem(resp.payload, "out") &&
	       cJSON_IsString(cJSON_GetObjectItem(resp.payload, "out")) &&
	       strcmp(cJSON_GetObjectItem(resp.payload, "out")->valuestring, "live-line") == 0);
	asap_envelope_clear(&env);
	asap_envelope_clear(&resp);
	tiny_http_join(&srv);
	return 0;
}

static int test_live_http_non_two_hundred(void)
{
	asap_envelope_t env;
	asap_envelope_t resp;
	struct tiny_http_srv srv;
	char err[256];
	char url[96];
	ASSERT(fill_min_task_request(&env) == 0);
	ASSERT(tiny_http_start(&srv, 418L, NULL, 0) == 0);
	asap_envelope_init(&resp);
	err[0] = '\0';
	ASSERT(snprintf(url, sizeof url, "http://127.0.0.1:%hu/", srv.bind_port) < (int)sizeof url);
	ASSERT(asap_client_send_task(url, NULL, ASAP_DEFAULT_JSONRPC_METHOD, &env, NULL, NULL, &resp,
				     err, sizeof err) == -1);
	ASSERT(strncmp(err, "HTTP", 4) == 0);
	asap_envelope_clear(&env);
	asap_envelope_clear(&resp);
	tiny_http_join(&srv);
	return 0;
}

static int test_live_http_malformed_result_envelope(void)
{
	asap_envelope_t env;
	asap_envelope_t resp;
	struct tiny_http_srv srv;
	char err[256];
	char url[96];
	ASSERT(fill_min_task_request(&env) == 0);
	ASSERT(tiny_http_start(&srv, 200L, jsonrpc_result_missing_payload, strlen(jsonrpc_result_missing_payload)) == 0);
	asap_envelope_init(&resp);
	err[0] = '\0';
	ASSERT(snprintf(url, sizeof url, "http://127.0.0.1:%hu/", srv.bind_port) < (int)sizeof url);
	ASSERT(asap_client_send_task(url, NULL, ASAP_DEFAULT_JSONRPC_METHOD, &env, NULL, NULL, &resp,
				     err, sizeof err) == -1);
	ASSERT(strstr(err, "payload") != NULL);
	asap_envelope_clear(&env);
	asap_envelope_clear(&resp);
	tiny_http_join(&srv);
	return 0;
}

static int test_empty_jsonrpc_method_uses_default(void)
{
	asap_envelope_t env;
	asap_envelope_t resp;
	struct tiny_http_srv srv;
	char err[512];
	char url[96];
	ASSERT(fill_min_task_request(&env) == 0);
	ASSERT(tiny_http_start(&srv, 200L, live_ok_body, strlen(live_ok_body)) == 0);
	asap_envelope_init(&resp);
	ASSERT(snprintf(url, sizeof url, "http://127.0.0.1:%hu/", srv.bind_port) < (int)sizeof url);
	err[0] = '\0';
	ASSERT(asap_client_send_task(url, NULL, "", &env, NULL, NULL, &resp, err, sizeof err) == 0);
	ASSERT(err[0] == '\0');
	asap_envelope_clear(&env);
	asap_envelope_clear(&resp);
	tiny_http_join(&srv);
	return 0;
}

static int test_send_with_explicit_jsonrpc_request_id(void)
{
	asap_envelope_t env;
	asap_envelope_t resp;
	struct tiny_http_srv srv;
	char err[256];
	char url[96];
	cJSON *req_id;
	ASSERT(fill_min_task_request(&env) == 0);
	req_id = cJSON_CreateString("fixed-req-tag");
	ASSERT(req_id != NULL);
	env.jsonrpc_request_id = req_id;
	ASSERT(tiny_http_start(&srv, 200L, live_ok_body_matching_req, strlen(live_ok_body_matching_req)) == 0);
	asap_envelope_init(&resp);
	ASSERT(snprintf(url, sizeof url, "http://127.0.0.1:%hu/", srv.bind_port) < (int)sizeof url);
	err[0] = '\0';
	ASSERT(asap_client_send_task(url, NULL, ASAP_DEFAULT_JSONRPC_METHOD, &env, NULL, NULL, &resp, err,
				     sizeof err) == 0);
	ASSERT(resp.jsonrpc_request_id != NULL);
	ASSERT(cJSON_IsString(resp.jsonrpc_request_id) &&
	       strcmp(resp.jsonrpc_request_id->valuestring, "fixed-req-tag") == 0);
	asap_envelope_clear(&env);
	asap_envelope_clear(&resp);
	tiny_http_join(&srv);
	return 0;
}

int main(int argc, char **argv)
{
	(void)argc;
	(void)argv;
	int failed = 0;
	curl_global_init(CURL_GLOBAL_DEFAULT);
	if (test_request_roundtrip_string() != 0) { fprintf(stderr, "test_request_roundtrip_string failed\n"); failed++; }
	if (test_parse_response_ok() != 0) { fprintf(stderr, "test_parse_response_ok failed\n"); failed++; }
	if (test_parse_response_jsonrpc_error() != 0) { fprintf(stderr, "test_parse_response_jsonrpc_error failed\n"); failed++; }
	if (test_parse_response_missing_payload() != 0) { fprintf(stderr, "test_parse_response_missing_payload failed\n"); failed++; }
	if (test_config_defaults() != 0) { fprintf(stderr, "test_config_defaults failed\n"); failed++; }
	if (test_config_from_config_timeout() != 0) { fprintf(stderr, "test_config_from_config_timeout failed\n"); failed++; }
	if (test_send_invalid_arguments() != 0) { fprintf(stderr, "test_send_invalid_arguments failed\n"); failed++; }
	if (test_live_http_roundtrip_success() != 0) { fprintf(stderr, "test_live_http_roundtrip_success failed\n"); failed++; }
	if (test_live_http_non_two_hundred() != 0) { fprintf(stderr, "test_live_http_non_two_hundred failed\n"); failed++; }
	if (test_live_http_malformed_result_envelope() != 0) { fprintf(stderr, "test_live_http_malformed_result_envelope failed\n"); failed++; }
	if (test_empty_jsonrpc_method_uses_default() != 0) { fprintf(stderr, "test_empty_jsonrpc_method_uses_default failed\n"); failed++; }
	if (test_send_with_explicit_jsonrpc_request_id() != 0) { fprintf(stderr, "test_send_with_explicit_jsonrpc_request_id failed\n"); failed++; }
	if (test_send_fails_no_server() != 0) { fprintf(stderr, "test_send_fails_no_server failed\n"); failed++; }
	curl_global_cleanup();
	if (failed == 0)
		printf("test_asap_client: all tests passed\n");
	return failed;
}
