/**
 * @file test_auth.c
 * @brief Unit tests for auth module: pairing code, token validation.
 */
#define _POSIX_C_SOURCE 200809L

#include "gateway/auth.h"
#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define ASSERT(c) do { if (!(c)) { fprintf(stderr, "FAIL: %s:%d %s\n", __FILE__, __LINE__, #c); return 1; } } while (0)
#define RUN(t) do { int r = (t); if (r) return r; } while (0)

/* Matches TOKEN_LEN / auth_pair cap in src/gateway/auth.c. */
#define TEST_TOKEN_HEX_LEN 32
#define TEST_AUTH_TOKEN_CAP 16

static void format_dummy_token(char *out, size_t out_size, int index)
{
	/* Deterministic 32-char hex so eviction of index 0 is observable. */
	snprintf(out, out_size, "%032d", index);
}

static int write_token_array(const char *path, int count)
{
	FILE *f;
	int i;
	char token[TEST_TOKEN_HEX_LEN + 1];

	f = fopen(path, "w");
	if (!f)
		return -1;
	fputc('[', f);
	for (i = 0; i < count; i++) {
		format_dummy_token(token, sizeof(token), i);
		if (i > 0)
			fputc(',', f);
		fprintf(f, "\"%s\"", token);
	}
	fputc(']', f);
	fclose(f);
	return 0;
}

static int read_token_array_size(const char *path)
{
	FILE *f;
	char buf[8192];
	size_t n;
	cJSON *root;
	int size;

	f = fopen(path, "r");
	if (!f)
		return -1;
	n = fread(buf, 1, sizeof(buf) - 1, f);
	fclose(f);
	buf[n] = '\0';
	root = cJSON_Parse(buf);
	if (!root || !cJSON_IsArray(root)) {
		if (root)
			cJSON_Delete(root);
		return -1;
	}
	size = cJSON_GetArraySize(root);
	cJSON_Delete(root);
	return size;
}

static int test_auth_init_cleanup(void)
{
	auth_ctx_t *ctx = auth_init(NULL);
	ASSERT(ctx != NULL);
	auth_cleanup(ctx);
	auth_cleanup(NULL);
	return 0;
}

static int test_auth_init_custom_path(void)
{
	auth_ctx_t *ctx = auth_init("/tmp/shellclaw_test_tokens.json");
	ASSERT(ctx != NULL);
	auth_cleanup(ctx);
	return 0;
}

static int test_auth_get_pairing_code_when_empty(void)
{
	unlink("/tmp/shellclaw_test_tokens_empty.json");
	auth_ctx_t *ctx = auth_init("/tmp/shellclaw_test_tokens_empty.json");
	ASSERT(ctx != NULL);
	char *code = auth_get_or_create_pairing_code(ctx);
	ASSERT(code != NULL);
	ASSERT(strlen(code) == 6);
	for (int i = 0; i < 6; i++)
		ASSERT(code[i] >= '0' && code[i] <= '9');
	free(code);
	auth_cleanup(ctx);
	return 0;
}

static int test_auth_pair_valid_code(void)
{
	unlink("/tmp/shellclaw_test_tokens_pair.json");
	auth_ctx_t *ctx = auth_init("/tmp/shellclaw_test_tokens_pair.json");
	ASSERT(ctx != NULL);
	char *code = auth_get_or_create_pairing_code(ctx);
	ASSERT(code != NULL);
	char token[64] = {0};
	int ret = auth_pair(ctx, code, token, sizeof(token));
	ASSERT(ret == 0);
	ASSERT(strlen(token) > 0);
	ASSERT(auth_validate_token(ctx, token) == 1);
	free(code);
	auth_cleanup(ctx);
	unlink("/tmp/shellclaw_test_tokens_pair.json");
	return 0;
}

static int test_auth_pair_invalid_code(void)
{
	unlink("/tmp/shellclaw_test_tokens_invalid.json");
	auth_ctx_t *ctx = auth_init("/tmp/shellclaw_test_tokens_invalid.json");
	ASSERT(ctx != NULL);
	char *code = auth_get_or_create_pairing_code(ctx);
	ASSERT(code != NULL);
	char token[64] = {0};
	int ret = auth_pair(ctx, "000000", token, sizeof(token));
	ASSERT(ret != 0);
	ret = auth_pair(ctx, code, token, sizeof(token));
	ASSERT(ret == 0);
	free(code);
	auth_cleanup(ctx);
	unlink("/tmp/shellclaw_test_tokens_invalid.json");
	return 0;
}

/* Fail closed when no pairing code was ever issued (tokens bootstrap skipped). */
static int test_auth_pair_rejects_without_pending_code(void)
{
	const char *path = "/tmp/shellclaw_test_tokens_no_pending.json";
	auth_ctx_t *ctx;
	char token[64];

	unlink(path);
	ctx = auth_init(path);
	ASSERT(ctx != NULL);
	memset(token, 0, sizeof(token));
	ASSERT(auth_pair(ctx, "123456", token, sizeof(token)) != 0);
	ASSERT(token[0] == '\0');
	ASSERT(auth_validate_token(ctx, "123456") == 0);
	auth_cleanup(ctx);
	unlink(path);
	return 0;
}

static int test_auth_validate_token(void)
{
	unlink("/tmp/shellclaw_test_tokens_validate.json");
	auth_ctx_t *ctx = auth_init("/tmp/shellclaw_test_tokens_validate.json");
	ASSERT(ctx != NULL);
	ASSERT(auth_validate_token(ctx, NULL) == 0);
	ASSERT(auth_validate_token(ctx, "") == 0);
	ASSERT(auth_validate_token(ctx, "invalid") == 0);
	char *code = auth_get_or_create_pairing_code(ctx);
	ASSERT(code != NULL);
	char token[64] = {0};
	auth_pair(ctx, code, token, sizeof(token));
	ASSERT(auth_validate_token(ctx, token) == 1);
	free(code);
	auth_cleanup(ctx);
	unlink("/tmp/shellclaw_test_tokens_validate.json");
	return 0;
}

static int test_auth_pairing_code_single_use(void)
{
	const char *path = "/tmp/shellclaw_test_tokens_singleuse.json";
	unlink(path);
	auth_ctx_t *ctx = auth_init(path);
	ASSERT(ctx != NULL);
	char *code = auth_get_or_create_pairing_code(ctx);
	ASSERT(code != NULL);
	char token[64] = {0};
	ASSERT(auth_pair(ctx, code, token, sizeof(token)) == 0);
	{
		char token2[64] = {0};
		ASSERT(auth_pair(ctx, code, token2, sizeof(token2)) != 0);
	}
	free(code);
	auth_cleanup(ctx);
	unlink(path);
	return 0;
}

static int test_auth_multi_token(void)
{
	const char *path = "/tmp/shellclaw_test_tokens_multi.json";
	unlink(path);
	/* First pairing. */
	auth_ctx_t *ctx1 = auth_init(path);
	ASSERT(ctx1 != NULL);
	char *code1 = auth_get_or_create_pairing_code(ctx1);
	ASSERT(code1 != NULL);
	char token1[64] = {0};
	ASSERT(auth_pair(ctx1, code1, token1, sizeof(token1)) == 0);
	ASSERT(auth_validate_token(ctx1, token1) == 1);
	free(code1);
	auth_cleanup(ctx1);
	/* Second pairing — need to delete tokens file to trigger new pairing code. */
	/* But we want to test that tokens accumulate, so we re-init and manually write
	   a state that allows a new pairing (empty tokens file but keep existing tokens
	   by writing them back after getting the code). Instead, test via validate: */
	/* Verify first token still works after file exists. */
	auth_ctx_t *ctx2 = auth_init(path);
	ASSERT(ctx2 != NULL);
	ASSERT(auth_validate_token(ctx2, token1) == 1);
	/* No new pairing available since file is non-empty (expected behavior). */
	char *code2 = auth_get_or_create_pairing_code(ctx2);
	ASSERT(code2 == NULL);
	auth_cleanup(ctx2);
	unlink(path);
	return 0;
}

static int test_pair_lockout_triggers_after_max_fails(void)
{
	int i;
	time_t now = 1000;
	auth_ctx_t *ctx = auth_init("/tmp/shellclaw_test_tokens_lockout.json");
	ASSERT(ctx != NULL);
	ASSERT(auth_pair_check_lockout(ctx, "10.0.0.1", now) == 0);
	for (i = 0; i < PAIR_LOCKOUT_MAX_FAILS; i++) {
		auth_pair_record_failure(ctx, "10.0.0.1", now);
		if (i < PAIR_LOCKOUT_MAX_FAILS - 1)
			ASSERT(auth_pair_check_lockout(ctx, "10.0.0.1", now) == 0);
	}
	ASSERT(auth_pair_check_lockout(ctx, "10.0.0.1", now) == 1);
	auth_cleanup(ctx);
	return 0;
}

static int test_pair_lockout_expires(void)
{
	time_t t0 = 2000;
	auth_ctx_t *ctx = auth_init("/tmp/shellclaw_test_tokens_lockout2.json");
	int i;
	ASSERT(ctx != NULL);
	for (i = 0; i < PAIR_LOCKOUT_MAX_FAILS; i++)
		auth_pair_record_failure(ctx, "10.0.0.2", t0);
	ASSERT(auth_pair_check_lockout(ctx, "10.0.0.2", t0) == 1);
	ASSERT(auth_pair_check_lockout(ctx, "10.0.0.2",
		t0 + PAIR_LOCKOUT_WINDOW_SECS) == 0);
	auth_cleanup(ctx);
	return 0;
}

static int test_pair_lockout_clear_on_success(void)
{
	time_t now = 3000;
	auth_ctx_t *ctx = auth_init("/tmp/shellclaw_test_tokens_lockout3.json");
	int i;
	ASSERT(ctx != NULL);
	for (i = 0; i < PAIR_LOCKOUT_MAX_FAILS - 1; i++)
		auth_pair_record_failure(ctx, "10.0.0.3", now);
	auth_pair_clear_ip(ctx, "10.0.0.3");
	for (i = 0; i < PAIR_LOCKOUT_MAX_FAILS - 1; i++)
		auth_pair_record_failure(ctx, "10.0.0.3", now);
	ASSERT(auth_pair_check_lockout(ctx, "10.0.0.3", now) == 0);
	auth_cleanup(ctx);
	return 0;
}

static int test_pair_lockout_independent_ips(void)
{
	time_t now = 4000;
	auth_ctx_t *ctx = auth_init("/tmp/shellclaw_test_tokens_lockout4.json");
	int i;
	ASSERT(ctx != NULL);
	for (i = 0; i < PAIR_LOCKOUT_MAX_FAILS; i++)
		auth_pair_record_failure(ctx, "10.0.0.4", now);
	ASSERT(auth_pair_check_lockout(ctx, "10.0.0.4", now) == 1);
	ASSERT(auth_pair_check_lockout(ctx, "10.0.0.5", now) == 0);
	auth_cleanup(ctx);
	return 0;
}

static int test_pair_lockout_null_ip(void)
{
	time_t now = 5000;
	auth_ctx_t *ctx = auth_init("/tmp/shellclaw_test_tokens_lockout5.json");
	int i;
	ASSERT(ctx != NULL);
	for (i = 0; i < PAIR_LOCKOUT_MAX_FAILS; i++)
		auth_pair_record_failure(ctx, NULL, now);
	ASSERT(auth_pair_check_lockout(ctx, NULL, now) == 1);
	ASSERT(auth_pair_check_lockout(ctx, "", now) == 1);
	auth_cleanup(ctx);
	return 0;
}

static int test_auth_pair_rejects_malformed_code(void)
{
	const char *path = "/tmp/shellclaw_test_tokens_malformed.json";
	auth_ctx_t *ctx;
	char *code;
	char token[64];

	unlink(path);
	ctx = auth_init(path);
	ASSERT(ctx != NULL);
	code = auth_get_or_create_pairing_code(ctx);
	ASSERT(code != NULL);

	memset(token, 0, sizeof(token));
	ASSERT(auth_pair(ctx, "12345", token, sizeof(token)) != 0);
	ASSERT(auth_pair(ctx, "1234567", token, sizeof(token)) != 0);
	ASSERT(auth_pair(ctx, "12a456", token, sizeof(token)) != 0);
	ASSERT(auth_pair(ctx, "", token, sizeof(token)) != 0);
	ASSERT(token[0] == '\0');

	/* Valid pending code still pairs after malformed rejects. */
	ASSERT(auth_pair(ctx, code, token, sizeof(token)) == 0);
	ASSERT(strlen(token) == TEST_TOKEN_HEX_LEN);
	ASSERT(auth_validate_token(ctx, token) == 1);

	free(code);
	auth_cleanup(ctx);
	unlink(path);
	return 0;
}

static int test_auth_pair_evicts_oldest_at_cap(void)
{
	const char *path = "/tmp/shellclaw_test_tokens_cap.json";
	auth_ctx_t *ctx;
	char *code;
	char token[64];
	char oldest[TEST_TOKEN_HEX_LEN + 1];
	char newest_seed[TEST_TOKEN_HEX_LEN + 1];

	unlink(path);
	ctx = auth_init(path);
	ASSERT(ctx != NULL);
	code = auth_get_or_create_pairing_code(ctx);
	ASSERT(code != NULL);

	/*
	 * Pairing requires a pending code from an empty/missing file, then we
	 * seed 16 tokens on disk before auth_pair appends (multi-device cap).
	 */
	ASSERT(write_token_array(path, TEST_AUTH_TOKEN_CAP) == 0);
	format_dummy_token(oldest, sizeof(oldest), 0);
	format_dummy_token(newest_seed, sizeof(newest_seed), TEST_AUTH_TOKEN_CAP - 1);
	ASSERT(auth_validate_token(ctx, oldest) == 1);
	ASSERT(auth_validate_token(ctx, newest_seed) == 1);

	memset(token, 0, sizeof(token));
	ASSERT(auth_pair(ctx, code, token, sizeof(token)) == 0);
	ASSERT(strlen(token) == TEST_TOKEN_HEX_LEN);
	ASSERT(auth_validate_token(ctx, token) == 1);
	ASSERT(read_token_array_size(path) == TEST_AUTH_TOKEN_CAP);
	ASSERT(auth_validate_token(ctx, oldest) == 0);
	ASSERT(auth_validate_token(ctx, newest_seed) == 1);

	free(code);
	auth_cleanup(ctx);
	unlink(path);
	return 0;
}

static int test_auth_validate_token_rejects_length_mismatch(void)
{
	const char *path = "/tmp/shellclaw_test_tokens_len.json";
	auth_ctx_t *ctx;
	char *code;
	char token[64];
	char longer[80];

	unlink(path);
	ctx = auth_init(path);
	ASSERT(ctx != NULL);
	code = auth_get_or_create_pairing_code(ctx);
	ASSERT(code != NULL);
	memset(token, 0, sizeof(token));
	ASSERT(auth_pair(ctx, code, token, sizeof(token)) == 0);
	ASSERT(auth_validate_token(ctx, token) == 1);

	snprintf(longer, sizeof(longer), "%sx", token);
	ASSERT(auth_validate_token(ctx, longer) == 0);

	free(code);
	auth_cleanup(ctx);
	unlink(path);
	return 0;
}

/** Must match LOCKOUT_TABLE_SIZE in src/gateway/auth.c */
#define TEST_LOCKOUT_TABLE_SIZE 32

static void lockout_fill_ip(auth_ctx_t *ctx, const char *ip, time_t now)
{
	int i;
	for (i = 0; i < PAIR_LOCKOUT_MAX_FAILS; i++)
		auth_pair_record_failure(ctx, ip, now);
}

/**
 * When the lockout table is full, lockout_find_or_create reuses slot 0.
 * That drops the first IP's lockout state so a 33rd attacker can proceed
 * while later slots keep their lockouts.
 */
static int test_pair_lockout_table_full_evicts_slot_zero(void)
{
	time_t now = 6000;
	auth_ctx_t *ctx = auth_init("/tmp/shellclaw_test_tokens_lockout6.json");
	char ip0[32];
	char ip1[32];
	char ip_last[32];
	char ip_overflow[32];
	int i;

	ASSERT(ctx != NULL);
	for (i = 0; i < TEST_LOCKOUT_TABLE_SIZE; i++) {
		char ip[32];
		snprintf(ip, sizeof(ip), "10.66.%d.%d", i / 256, i % 256);
		lockout_fill_ip(ctx, ip, now);
	}

	snprintf(ip0, sizeof(ip0), "10.66.0.0");
	snprintf(ip1, sizeof(ip1), "10.66.0.1");
	snprintf(ip_last, sizeof(ip_last), "10.66.0.%d", TEST_LOCKOUT_TABLE_SIZE - 1);
	snprintf(ip_overflow, sizeof(ip_overflow), "10.66.1.0");

	ASSERT(auth_pair_check_lockout(ctx, ip0, now) == 1);
	ASSERT(auth_pair_check_lockout(ctx, ip1, now) == 1);
	ASSERT(auth_pair_check_lockout(ctx, ip_last, now) == 1);

	lockout_fill_ip(ctx, ip_overflow, now);

	/* Slot 1+ must keep lockouts; only slot 0 was reused for the 33rd IP. */
	ASSERT(auth_pair_check_lockout(ctx, ip1, now) == 1);
	ASSERT(auth_pair_check_lockout(ctx, ip_last, now) == 1);
	ASSERT(auth_pair_check_lockout(ctx, ip_overflow, now) == 1);
	/* Evicted IP loses prior lockout (check recreates a fresh slot-0 entry). */
	ASSERT(auth_pair_check_lockout(ctx, ip0, now) == 0);

	auth_cleanup(ctx);
	return 0;
}

int main(void)
{
	int failed = 0;
	if (test_auth_init_cleanup() != 0) { fprintf(stderr, "test_auth_init_cleanup failed\n"); failed++; }
	if (test_auth_init_custom_path() != 0) { fprintf(stderr, "test_auth_init_custom_path failed\n"); failed++; }
	if (test_auth_get_pairing_code_when_empty() != 0) { fprintf(stderr, "test_auth_get_pairing_code_when_empty failed\n"); failed++; }
	if (test_auth_pair_valid_code() != 0) { fprintf(stderr, "test_auth_pair_valid_code failed\n"); failed++; }
	if (test_auth_pair_invalid_code() != 0) { fprintf(stderr, "test_auth_pair_invalid_code failed\n"); failed++; }
	if (test_auth_pairing_code_single_use() != 0) { fprintf(stderr, "test_auth_pairing_code_single_use failed\n"); failed++; }
	if (test_auth_pair_rejects_without_pending_code() != 0) { fprintf(stderr, "test_auth_pair_rejects_without_pending_code failed\n"); failed++; }
	if (test_auth_validate_token() != 0) { fprintf(stderr, "test_auth_validate_token failed\n"); failed++; }
	if (test_auth_multi_token() != 0) { fprintf(stderr, "test_auth_multi_token failed\n"); failed++; }
	if (test_pair_lockout_triggers_after_max_fails() != 0) { fprintf(stderr, "test_pair_lockout_triggers_after_max_fails failed\n"); failed++; }
	if (test_pair_lockout_expires() != 0) { fprintf(stderr, "test_pair_lockout_expires failed\n"); failed++; }
	if (test_pair_lockout_clear_on_success() != 0) { fprintf(stderr, "test_pair_lockout_clear_on_success failed\n"); failed++; }
	if (test_pair_lockout_independent_ips() != 0) { fprintf(stderr, "test_pair_lockout_independent_ips failed\n"); failed++; }
	if (test_pair_lockout_null_ip() != 0) { fprintf(stderr, "test_pair_lockout_null_ip failed\n"); failed++; }
	if (test_auth_pair_rejects_malformed_code() != 0) { fprintf(stderr, "test_auth_pair_rejects_malformed_code failed\n"); failed++; }
	if (test_auth_pair_evicts_oldest_at_cap() != 0) { fprintf(stderr, "test_auth_pair_evicts_oldest_at_cap failed\n"); failed++; }
	if (test_auth_validate_token_rejects_length_mismatch() != 0) { fprintf(stderr, "test_auth_validate_token_rejects_length_mismatch failed\n"); failed++; }
	if (test_pair_lockout_table_full_evicts_slot_zero() != 0) { fprintf(stderr, "test_pair_lockout_table_full_evicts_slot_zero failed\n"); failed++; }
	if (failed == 0)
		printf("test_auth: all tests passed\n");
	return failed;
}
