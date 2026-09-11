/**
 * @file test_asap_http_body.c
 * @brief Unit tests for POST /asap body buffer and Content-Length parsing.
 */
#define _POSIX_C_SOURCE 200809L

#include "gateway/asap_http_body.h"
#include <stdio.h>
#include <string.h>

#define ASSERT(c) do { \
	if (!(c)) { \
		fprintf(stderr, "FAIL: %s:%d %s\n", __FILE__, __LINE__, #c); \
		return 1; \
	} \
} while (0)

static int test_parse_content_length_valid(void)
{
	long cl = 0;
	ASSERT(asap_http_body_parse_content_length("1024", &cl) == 0);
	ASSERT(cl == 1024);
	return 0;
}

static int test_parse_content_length_rejects(void)
{
	long cl = 0;
	ASSERT(asap_http_body_parse_content_length("", &cl) != 0);
	ASSERT(asap_http_body_parse_content_length("-1", &cl) != 0);
	ASSERT(asap_http_body_parse_content_length("1e6", &cl) != 0);
	ASSERT(asap_http_body_parse_content_length("1048577", &cl) != 0);
	return 0;
}

static int test_dyn_append_sets_too_large(void)
{
	asap_http_body_t body;
	const char payload[] = "0123456789abcdef";
	char *dyn;

	memset(&body, 0, sizeof(body));
	dyn = malloc(11);
	ASSERT(dyn != NULL);
	body.body_dyn = dyn;
	body.body_dyn_cap = 10;
	body.body_dyn_len = 0;
	body.use_dyn_body = 1;
	asap_http_body_append(&body, payload, sizeof(payload) - 1);
	ASSERT(body.body_too_large == 1);
	ASSERT(body.body_dyn_len == 10);
	ASSERT(strcmp(body.body_dyn, "0123456789") == 0);
	asap_http_body_free(&body);
	return 0;
}

static int test_static_append_sets_too_large(void)
{
	asap_http_body_t body;

	memset(&body, 0, sizeof(body));
	memset(body.body, 'x', BODY_BUF_SIZE - 1);
	body.body_len = BODY_BUF_SIZE - 1;
	body.body[BODY_BUF_SIZE - 1] = '\0';
	asap_http_body_append(&body, "z", 1);
	ASSERT(body.body_too_large == 1);
	return 0;
}

int main(void)
{
	int failed = 0;
	if (test_parse_content_length_valid() != 0) {
		fprintf(stderr, "test_parse_content_length_valid failed\n");
		failed++;
	}
	if (test_parse_content_length_rejects() != 0) {
		fprintf(stderr, "test_parse_content_length_rejects failed\n");
		failed++;
	}
	if (test_dyn_append_sets_too_large() != 0) {
		fprintf(stderr, "test_dyn_append_sets_too_large failed\n");
		failed++;
	}
	if (test_static_append_sets_too_large() != 0) {
		fprintf(stderr, "test_static_append_sets_too_large failed\n");
		failed++;
	}
	if (failed == 0)
		printf("test_asap_http_body: all tests passed\n");
	return failed ? 1 : 0;
}
