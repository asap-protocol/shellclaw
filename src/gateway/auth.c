/**
 * @file auth.c
 * @brief Pairing code generation, bearer token store, and /pair brute-force lockout.
 */
#if defined(__APPLE__)
#define _DARWIN_C_SOURCE
#endif
#define _POSIX_C_SOURCE 200809L

#include "gateway/auth.h"
#include "core/config.h"
#include "crypto/crypto.h"
#include "cJSON.h"
#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <limits.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_TOKENS_PATH "~/.shellclaw/auth_tokens.json"
#define PAIRING_CODE_LEN 6
#define TOKEN_LEN 32

#define LOCKOUT_TABLE_SIZE 32
#define LOCKOUT_IP_SIZE 48

typedef struct pair_lockout_entry {
	char ip[LOCKOUT_IP_SIZE];
	int fail_count;
	time_t locked_until;
} pair_lockout_entry_t;

struct auth_ctx {
	char *tokens_path;
	char *pending_pairing_code;
	pair_lockout_entry_t lockout[LOCKOUT_TABLE_SIZE];
};

static int is_file_empty_or_missing(const char *path)
{
	FILE *f = fopen(path, "r");
	if (!f) return 1;
	int c = fgetc(f);
	fclose(f);
	return (c == EOF);
}

static int ensure_tokens_dir(const char *path);

static int generate_random_hex(char *out, size_t len)
{
	if (!out || len == 0) return -1;
	size_t raw_len = (len + 1) / 2;
	unsigned char *raw = malloc(raw_len);
	if (!raw) return -1;
	if (crypto_read_urandom(raw, raw_len) != 0) {
		free(raw);
		return -1;
	}
	for (size_t i = 0; i < len; i++) {
		int v = (i % 2 == 0) ? (raw[i / 2] >> 4) : (raw[i / 2] & 0x0F);
		out[i] = (char)(v < 10 ? '0' + v : 'a' + v - 10);
	}
	out[len] = '\0';
	free(raw);
	return 0;
}

static int generate_pairing_code(char *out, size_t out_size)
{
	if (!out || out_size < (size_t)(PAIRING_CODE_LEN + 1)) return -1;
	unsigned char raw[PAIRING_CODE_LEN];
	if (crypto_read_urandom(raw, sizeof(raw)) != 0) return -1;
	for (int i = 0; i < PAIRING_CODE_LEN; i++)
		out[i] = (char)('0' + (raw[i] % 10));
	out[PAIRING_CODE_LEN] = '\0';
	return 0;
}

static int constant_time_cmp(const char *a, const char *b, size_t len)
{
	volatile unsigned char result = 0;
	for (size_t i = 0; i < len; i++)
		result |= (unsigned char)a[i] ^ (unsigned char)b[i];
	return result == 0;
}

static int is_valid_6digit(const char *code)
{
	if (!code) return 0;
	for (int i = 0; i < PAIRING_CODE_LEN; i++) {
		if (code[i] < '0' || code[i] > '9') return 0;
	}
	return code[PAIRING_CODE_LEN] == '\0';
}

auth_ctx_t *auth_init(const char *tokens_path)
{
	auth_ctx_t *ctx = calloc(1, sizeof(*ctx));
	if (!ctx) return NULL;
	if (tokens_path && tokens_path[0] != '\0') {
		ctx->tokens_path = strdup(tokens_path);
	} else {
		ctx->tokens_path = config_expand_tilde(DEFAULT_TOKENS_PATH);
	}
	if (!ctx->tokens_path) {
		free(ctx);
		return NULL;
	}
	return ctx;
}

void auth_cleanup(auth_ctx_t *ctx)
{
	if (!ctx) return;
	free(ctx->tokens_path);
	free(ctx->pending_pairing_code);
	free(ctx);
}

char *auth_get_or_create_pairing_code(auth_ctx_t *ctx)
{
	if (!ctx || !ctx->tokens_path) return NULL;
	if (!is_file_empty_or_missing(ctx->tokens_path)) return NULL;
	free(ctx->pending_pairing_code);
	ctx->pending_pairing_code = NULL;
	char code[PAIRING_CODE_LEN + 1];
	if (generate_pairing_code(code, sizeof(code)) != 0) return NULL;
	ctx->pending_pairing_code = strdup(code);
	if (!ctx->pending_pairing_code) return NULL;
	printf("ShellClaw pairing code: %s\n", code);
	fflush(stdout);
	if (getenv("SHELLCLAW_TEST_MODE")) {
		const char *home = getenv("HOME");
		if (home && home[0] != '\0') {
			char test_path[512];
			snprintf(test_path, sizeof(test_path), "%s/.shellclaw/test_pairing_code", home);
			if (ensure_tokens_dir(test_path) == 0) {
				FILE *tf = fopen(test_path, "w");
				if (tf) {
					fprintf(tf, "%s\n", code);
					fclose(tf);
				}
			}
		}
	}
	return strdup(code);
}

static int ensure_tokens_dir(const char *path)
{
	char *copy = strdup(path);
	if (!copy) return -1;
	char *slash = strrchr(copy, '/');
	if (slash) {
		*slash = '\0';
		if (slash != copy) {
			if (mkdir(copy, 0700) != 0 && errno != EEXIST) {
				free(copy);
				return -1;
			}
		}
	}
	free(copy);
	return 0;
}

static void discard_tokens_tmp(int fd, const char *tmp_path)
{
	if (fd >= 0)
		(void)close(fd);
	if (tmp_path && tmp_path[0] != '\0')
		(void)unlink(tmp_path);
}

static int auth_write_all(int fd, const char *buf, size_t len)
{
	size_t off = 0;

	while (off < len) {
		ssize_t n = write(fd, buf + off, len - off);
		if (n <= 0)
			return -1;
		off += (size_t)n;
	}
	return 0;
}

/*
 * Unique temp+rename so O_TRUNC cannot wipe auth_tokens.json before the new
 * JSON is fully on disk (ENOSPC / crash / fdopen failure). mkstemp uses O_EXCL
 * so a planted path.tmp symlink is not followed (the #90 shape).
 */
static int write_tokens_atomic(const char *path, const char *json)
{
	char path_copy[PATH_MAX];
	char tmp_path[PATH_MAX];
	char *dir;
	int fd;
	int n;

	if (!path || !json)
		return -1;
	if (snprintf(path_copy, sizeof(path_copy), "%s", path) >= (int)sizeof(path_copy))
		return -1;
	dir = dirname(path_copy);
	if (!dir || dir[0] == '\0')
		return -1;
	n = snprintf(tmp_path, sizeof(tmp_path), "%s/.sc-auth-XXXXXX", dir);
	if (n < 0 || (size_t)n >= sizeof(tmp_path))
		return -1;
	fd = mkstemp(tmp_path);
	if (fd < 0)
		return -1;
	(void)fcntl(fd, F_SETFD, FD_CLOEXEC);
	if (auth_write_all(fd, json, strlen(json)) != 0) {
		discard_tokens_tmp(fd, tmp_path);
		return -1;
	}
	if (fsync(fd) != 0) {
		discard_tokens_tmp(fd, tmp_path);
		return -1;
	}
	if (close(fd) != 0) {
		discard_tokens_tmp(-1, tmp_path);
		return -1;
	}
	if (rename(tmp_path, path) != 0) {
		discard_tokens_tmp(-1, tmp_path);
		return -1;
	}
	return 0;
}

int auth_pair(auth_ctx_t *ctx, const char *code, char *token_out, size_t token_size)
{
	if (!ctx || !ctx->tokens_path || !code || !token_out || token_size == 0) return -1;
	if (!is_valid_6digit(code)) return -1;
	if (!ctx->pending_pairing_code ||
	    !constant_time_cmp(code, ctx->pending_pairing_code, PAIRING_CODE_LEN))
		return -1;
	char new_token[TOKEN_LEN + 1];
	/* Fail closed: never persist or return an uninitialized bearer on RNG/OOM. */
	if (generate_random_hex(new_token, TOKEN_LEN) != 0)
		return -1;
	/* Read existing tokens and append (multi-device support). */
	cJSON *arr = NULL;
	{
		FILE *f = fopen(ctx->tokens_path, "r");
		if (f) {
			char buf[8192];
			size_t n = fread(buf, 1, sizeof(buf) - 1, f);
			fclose(f);
			buf[n] = '\0';
			cJSON *existing = cJSON_Parse(buf);
			if (existing && cJSON_IsArray(existing))
				arr = existing;
			else if (existing)
				cJSON_Delete(existing);
		}
	}
	if (!arr) arr = cJSON_CreateArray();
	if (!arr) return -1;
	/* Cap at 16 tokens to prevent unbounded growth. */
	while (cJSON_GetArraySize(arr) >= 16)
		cJSON_DeleteItemFromArray(arr, 0);
	cJSON_AddItemToArray(arr, cJSON_CreateString(new_token));
	char *json = cJSON_PrintUnformatted(arr);
	cJSON_Delete(arr);
	if (!json) return -1;
	if (ensure_tokens_dir(ctx->tokens_path) != 0) {
		free(json);
		return -1;
	}
	if (write_tokens_atomic(ctx->tokens_path, json) != 0) {
		free(json);
		return -1;
	}
	free(json);
	size_t copy_len = (size_t)TOKEN_LEN < token_size - 1 ? (size_t)TOKEN_LEN : token_size - 1;
	memcpy(token_out, new_token, copy_len);
	token_out[copy_len] = '\0';
	free(ctx->pending_pairing_code);
	ctx->pending_pairing_code = NULL;
	return 0;
}

int auth_validate_token(auth_ctx_t *ctx, const char *token)
{
	if (!ctx || !ctx->tokens_path || !token || token[0] == '\0') return 0;
	FILE *f = fopen(ctx->tokens_path, "r");
	if (!f) return 0;
	char buf[8192];
	size_t n = fread(buf, 1, sizeof(buf) - 1, f);
	fclose(f);
	buf[n] = '\0';
	cJSON *root = cJSON_Parse(buf);
	if (!root || !cJSON_IsArray(root)) {
		if (root) cJSON_Delete(root);
		return 0;
	}
	int found = 0;
	int count = cJSON_GetArraySize(root);
	size_t tok_len = strlen(token);
	for (int i = 0; i < count && !found; i++) {
		cJSON *item = cJSON_GetArrayItem(root, i);
		if (cJSON_IsString(item) && item->valuestring &&
		    strlen(item->valuestring) == tok_len &&
		    constant_time_cmp(item->valuestring, token, tok_len))
			found = 1;
	}
	cJSON_Delete(root);
	return found ? 1 : 0;
}

static pair_lockout_entry_t *lockout_find_or_create(auth_ctx_t *ctx, const char *ip)
{
	int i;
	int free_slot = -1;
	for (i = 0; i < LOCKOUT_TABLE_SIZE; i++) {
		if (ctx->lockout[i].ip[0] == '\0') {
			if (free_slot < 0) free_slot = i;
			continue;
		}
		if (strncmp(ctx->lockout[i].ip, ip, LOCKOUT_IP_SIZE - 1) == 0)
			return &ctx->lockout[i];
	}
	if (free_slot >= 0) {
		pair_lockout_entry_t *e = &ctx->lockout[free_slot];
		strncpy(e->ip, ip, LOCKOUT_IP_SIZE - 1);
		e->ip[LOCKOUT_IP_SIZE - 1] = '\0';
		e->fail_count = 0;
		e->locked_until = 0;
		return e;
	}
	/* Table full: reuse slot 0 (evict oldest without LRU overhead). */
	pair_lockout_entry_t *e = &ctx->lockout[0];
	strncpy(e->ip, ip, LOCKOUT_IP_SIZE - 1);
	e->ip[LOCKOUT_IP_SIZE - 1] = '\0';
	e->fail_count = 0;
	e->locked_until = 0;
	return e;
}

int auth_pair_check_lockout(auth_ctx_t *ctx, const char *ip, time_t now)
{
	const char *safe_ip;
	pair_lockout_entry_t *e;
	if (!ctx) return 0;
	safe_ip = (ip && ip[0] != '\0') ? ip : "unknown";
	e = lockout_find_or_create(ctx, safe_ip);
	if (e->locked_until > 0 && now < e->locked_until) return 1;
	if (e->locked_until > 0 && now >= e->locked_until) {
		e->fail_count = 0;
		e->locked_until = 0;
	}
	return 0;
}

void auth_pair_record_failure(auth_ctx_t *ctx, const char *ip, time_t now)
{
	const char *safe_ip;
	pair_lockout_entry_t *e;
	if (!ctx) return;
	safe_ip = (ip && ip[0] != '\0') ? ip : "unknown";
	e = lockout_find_or_create(ctx, safe_ip);
	e->fail_count++;
	if (e->fail_count >= PAIR_LOCKOUT_MAX_FAILS)
		e->locked_until = now + PAIR_LOCKOUT_WINDOW_SECS;
}

void auth_pair_clear_ip(auth_ctx_t *ctx, const char *ip)
{
	const char *safe_ip;
	pair_lockout_entry_t *e;
	if (!ctx) return;
	safe_ip = (ip && ip[0] != '\0') ? ip : "unknown";
	e = lockout_find_or_create(ctx, safe_ip);
	e->fail_count = 0;
	e->locked_until = 0;
}
