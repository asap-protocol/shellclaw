/**
 * @file test_telegram.c
 * @brief Unit tests for Telegram channel: parse_update, allowlist, edge cases.
 */
#define _POSIX_C_SOURCE 200809L

#include "channels/channel.h"
#include "core/config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Test-only entry points from telegram.c (compiled with -DSHELLCLAW_TEST) */
extern int telegram_parse_update_for_test(const char *json, channel_incoming_msg_t *out, long *update_id);
extern void telegram_set_test_config(const config_t *cfg);
extern int telegram_format_getupdates_url_for_test(char *url, size_t url_cap, const char *token,
                                                   int timeout_sec, long offset);
extern int telegram_response_ok_for_test(long http_code, const char *body);

static int tests_run = 0;
static int tests_failed = 0;

#define MU_ASSERT(cond, msg) do { \
	tests_run++; \
	if (!(cond)) { \
		fprintf(stderr, "FAIL: %s\n", (msg)); \
		tests_failed++; \
		return; \
	} \
} while (0)

#define MU_RUN(test) do { test(); } while (0)

/* --- Helpers --- */

static config_t *load_test_config(const char *path, const char *allowed_user)
{
	FILE *f = fopen(path, "w");
	if (!f) return NULL;
	if (allowed_user)
		fprintf(f, "[agent]\nmodel = \"test\"\n[channels.telegram]\nenabled = true\ntoken_env = \"TG_TOK\"\nallowed_users = [\"%s\"]\n", allowed_user);
	else
		fprintf(f, "[agent]\nmodel = \"test\"\n[channels.telegram]\nenabled = true\ntoken_env = \"TG_TOK\"\nallowed_users = []\n");
	fclose(f);
	config_t *cfg = NULL;
	char errbuf[256];
	if (config_load(path, &cfg, errbuf, sizeof(errbuf)) != 0) return NULL;
	return cfg;
}

/* --- Tests --- */

static void test_parse_valid_text_message(void)
{
	const char *json =
		"{\"ok\":true,\"result\":[{"
		"\"update_id\":12345,"
		"\"message\":{\"from\":{\"id\":42},\"text\":\"Hello bot\"}"
		"}]}";
	config_t *cfg = load_test_config("/tmp/test_tg_parse.toml", "42");
	MU_ASSERT(cfg != NULL, "load config");
	telegram_set_test_config(cfg);

	channel_incoming_msg_t msg;
	memset(&msg, 0, sizeof(msg));
	long uid = 0;
	int r = telegram_parse_update_for_test(json, &msg, &uid);
	MU_ASSERT(r == 1, "parse returns 1 for valid message");
	MU_ASSERT(uid == 12345, "update_id parsed");
	MU_ASSERT(msg.session_id != NULL, "session_id set");
	MU_ASSERT(strstr(msg.session_id, "telegram:42") != NULL, "session_id contains telegram:42");
	MU_ASSERT(msg.user_id != NULL, "user_id set");
	MU_ASSERT(strcmp(msg.user_id, "42") == 0, "user_id is 42");
	MU_ASSERT(msg.text != NULL, "text set");
	MU_ASSERT(strcmp(msg.text, "Hello bot") == 0, "text matches");
	MU_ASSERT(msg.attachments_count == 0, "no attachments");

	channel_incoming_msg_clear(&msg);
	config_free(cfg);
	remove("/tmp/test_tg_parse.toml");
}

static void test_parse_empty_result_array(void)
{
	const char *json = "{\"ok\":true,\"result\":[]}";
	config_t *cfg = load_test_config("/tmp/test_tg_empty.toml", "42");
	MU_ASSERT(cfg != NULL, "load config");
	telegram_set_test_config(cfg);

	channel_incoming_msg_t msg;
	memset(&msg, 0, sizeof(msg));
	long uid = 0;
	int r = telegram_parse_update_for_test(json, &msg, &uid);
	MU_ASSERT(r == 0, "empty result returns 0");

	channel_incoming_msg_clear(&msg);
	config_free(cfg);
	remove("/tmp/test_tg_empty.toml");
}

static void test_parse_invalid_json(void)
{
	config_t *cfg = load_test_config("/tmp/test_tg_bad.toml", "42");
	MU_ASSERT(cfg != NULL, "load config");
	telegram_set_test_config(cfg);

	channel_incoming_msg_t msg;
	memset(&msg, 0, sizeof(msg));
	long uid = 0;
	int r = telegram_parse_update_for_test("not json at all", &msg, &uid);
	MU_ASSERT(r == -1, "invalid JSON returns -1");

	channel_incoming_msg_clear(&msg);
	config_free(cfg);
	remove("/tmp/test_tg_bad.toml");
}

static void test_parse_user_not_allowed(void)
{
	const char *json =
		"{\"ok\":true,\"result\":[{"
		"\"update_id\":100,"
		"\"message\":{\"from\":{\"id\":999},\"text\":\"spam\"}"
		"}]}";
	config_t *cfg = load_test_config("/tmp/test_tg_deny.toml", "42");
	MU_ASSERT(cfg != NULL, "load config");
	telegram_set_test_config(cfg);

	channel_incoming_msg_t msg;
	memset(&msg, 0, sizeof(msg));
	long uid = 0;
	int r = telegram_parse_update_for_test(json, &msg, &uid);
	MU_ASSERT(r == 0, "non-allowed user returns 0 (skip)");
	MU_ASSERT(uid == 100, "update_id still parsed");
	MU_ASSERT(msg.text == NULL, "text not set for denied user");

	channel_incoming_msg_clear(&msg);
	config_free(cfg);
	remove("/tmp/test_tg_deny.toml");
}

static void test_parse_photo_with_caption(void)
{
	const char *json =
		"{\"ok\":true,\"result\":[{"
		"\"update_id\":200,"
		"\"message\":{\"from\":{\"id\":42},"
		"\"photo\":[{\"file_id\":\"small\",\"width\":90},{\"file_id\":\"large_id\",\"width\":800}],"
		"\"caption\":\"photo caption\"}"
		"}]}";
	config_t *cfg = load_test_config("/tmp/test_tg_photo.toml", "42");
	MU_ASSERT(cfg != NULL, "load config");
	telegram_set_test_config(cfg);

	channel_incoming_msg_t msg;
	memset(&msg, 0, sizeof(msg));
	long uid = 0;
	int r = telegram_parse_update_for_test(json, &msg, &uid);
	MU_ASSERT(r == 1, "photo message parsed");
	MU_ASSERT(msg.attachments_count == 1, "one attachment");
	MU_ASSERT(msg.attachments != NULL, "attachments allocated");
	MU_ASSERT(strcmp(msg.attachments[0].path_or_base64, "large_id") == 0, "largest photo selected");
	MU_ASSERT(msg.text != NULL, "text from caption");
	MU_ASSERT(strcmp(msg.text, "photo caption") == 0, "caption used as text");

	channel_incoming_msg_clear(&msg);
	config_free(cfg);
	remove("/tmp/test_tg_photo.toml");
}

static void test_parse_no_message_key(void)
{
	const char *json =
		"{\"ok\":true,\"result\":[{\"update_id\":300}]}";
	config_t *cfg = load_test_config("/tmp/test_tg_nomsg.toml", "42");
	MU_ASSERT(cfg != NULL, "load config");
	telegram_set_test_config(cfg);

	channel_incoming_msg_t msg;
	memset(&msg, 0, sizeof(msg));
	long uid = 0;
	int r = telegram_parse_update_for_test(json, &msg, &uid);
	MU_ASSERT(r == 0, "update without message returns 0");
	MU_ASSERT(uid == 300, "update_id still parsed");

	channel_incoming_msg_clear(&msg);
	config_free(cfg);
	remove("/tmp/test_tg_nomsg.toml");
}

static void test_parse_missing_from_field(void)
{
	const char *json =
		"{\"ok\":true,\"result\":[{"
		"\"update_id\":400,"
		"\"message\":{\"text\":\"no from\"}"
		"},{"
		"\"update_id\":401,"
		"\"message\":{\"from\":{\"id\":42},\"text\":\"kept\"}"
		"}]}";
	config_t *cfg = load_test_config("/tmp/test_tg_nofrom.toml", "42");
	MU_ASSERT(cfg != NULL, "load config");
	telegram_set_test_config(cfg);

	channel_incoming_msg_t msg;
	memset(&msg, 0, sizeof(msg));
	long uid = 0;
	int r = telegram_parse_update_for_test(json, &msg, &uid);
	/* Skip, but keep this update_id so the next poll can fetch 401.
	 * Returning -1 left the offset unchanged and pinned the inbox. */
	MU_ASSERT(r == 0, "missing 'from' is skipped");
	MU_ASSERT(uid == 400, "offset stays on the skipped update");
	MU_ASSERT(msg.text == NULL, "skipped update is not delivered");

	channel_incoming_msg_clear(&msg);
	config_free(cfg);
	remove("/tmp/test_tg_nofrom.toml");
}

static void test_getupdates_url_requests_one_update(void)
{
	char url[512];
	int rc = telegram_format_getupdates_url_for_test(url, sizeof(url), "tok", 1, 99);
	MU_ASSERT(rc == 0, "getUpdates url built");
	MU_ASSERT(strstr(url, "/bottok/getUpdates?") != NULL, "token and method in url");
	MU_ASSERT(strstr(url, "offset=99") != NULL, "offset preserved");
	MU_ASSERT(strstr(url, "limit=1") != NULL, "batch capped at one update");
}

static void test_send_response_requires_ok(void)
{
	const char *too_long = "{\"ok\":false,\"error_code\":400,"
		"\"description\":\"Bad Request: message is too long\"}";
	const char *accepted = "{\"ok\":true,\"result\":{\"message_id\":1}}";

	MU_ASSERT(telegram_response_ok_for_test(400, too_long) == 0,
		"HTTP 400 message-too-long is not delivery");
	MU_ASSERT(telegram_response_ok_for_test(200, "{\"ok\":false}") == 0,
		"HTTP 200 with ok false is not delivery");
	MU_ASSERT(telegram_response_ok_for_test(200, "") == 0,
		"empty body is not delivery");
	MU_ASSERT(telegram_response_ok_for_test(429, "{\"ok\":false}") == 0,
		"HTTP 429 is not delivery");
	MU_ASSERT(telegram_response_ok_for_test(200, accepted) == 1,
		"HTTP 200 with ok true is delivery");
}

static void test_vtable_fields_set(void)
{
	const channel_t *ch = channel_telegram_get();
	MU_ASSERT(ch != NULL, "channel_telegram_get not null");
	MU_ASSERT(ch->name != NULL && strcmp(ch->name, "telegram") == 0, "name is 'telegram'");
	MU_ASSERT(ch->init != NULL, "init not null");
	MU_ASSERT(ch->poll != NULL, "poll not null");
	MU_ASSERT(ch->send != NULL, "send not null");
	MU_ASSERT(ch->cleanup != NULL, "cleanup not null");
}

int main(void)
{
	MU_RUN(test_parse_valid_text_message);
	MU_RUN(test_parse_empty_result_array);
	MU_RUN(test_parse_invalid_json);
	MU_RUN(test_parse_user_not_allowed);
	MU_RUN(test_parse_photo_with_caption);
	MU_RUN(test_parse_no_message_key);
	MU_RUN(test_parse_missing_from_field);
MU_RUN(test_getupdates_url_requests_one_update);
	MU_RUN(test_send_response_requires_ok);
	MU_RUN(test_vtable_fields_set);
	printf("%d tests run, %d failed\n", tests_run, tests_failed);
	return tests_failed ? 1 : 0;
}
