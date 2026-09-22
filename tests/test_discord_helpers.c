/**
 * @file test_discord_helpers.c
 * @brief Unit tests for Discord pure helpers (no bot token, no network).
 */

#include "channels/discord_helpers.h"
#include "channels/channel.h"
#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ASSERT(c) do { if (!(c)) { fprintf(stderr, "FAIL: %s:%d %s\n", __FILE__, __LINE__, #c); return 1; } } while (0)
#define RUN(t) do { int _r = (t); if (_r) return _r; } while (0)

static int test_allow_entry_equals(void)
{
	ASSERT(discord_helpers_allow_entry_equals("111", "111") == 1);
	ASSERT(discord_helpers_allow_entry_equals("111", "222") == 0);
	ASSERT(discord_helpers_allow_entry_equals(NULL, "111") == 0);
	ASSERT(discord_helpers_allow_entry_equals("", "111") == 0);
	return 0;
}

static int test_allowlist_array(void)
{
	const char *allowed[] = { "10", "20", "30" };
	ASSERT(discord_helpers_user_in_allowlist(allowed, 3, "20") == 1);
	ASSERT(discord_helpers_user_in_allowlist(allowed, 3, "99") == 0);
	ASSERT(discord_helpers_user_in_allowlist(allowed, 0, "10") == 0);
	ASSERT(discord_helpers_user_in_allowlist(NULL, 3, "10") == 0);
	return 0;
}

static int test_session_id_format(void)
{
	char buf[128];
	ASSERT(discord_helpers_session_id_from_channel("9876543210", buf, sizeof(buf)) == 0);
	ASSERT(strcmp(buf, "discord:c:9876543210") == 0);
	ASSERT(discord_helpers_session_id_from_channel("9876543210", buf, 12) != 0);
	return 0;
}

static int test_mentions_fixture(void)
{
	const char *fixture = "[{\"id\":\"bot-user-1\",\"username\":\"shell\"},{\"id\":\"other\",\"username\":\"x\"}]";
	cJSON *arr = cJSON_Parse(fixture);
	ASSERT(arr != NULL);
	ASSERT(discord_helpers_mentions_include_bot(arr, "bot-user-1") == 1);
	ASSERT(discord_helpers_mentions_include_bot(arr, "absent") == 0);
	cJSON_Delete(arr);
	ASSERT(discord_helpers_mentions_include_bot(NULL, "bot-user-1") == 0);
	return 0;
}

static int test_backoff_math(void)
{
	int ms;
	ms = discord_helpers_send_backoff_ms(2, -1.0, 10, 400, 300000);
	ASSERT(ms == (4 * 400 + 10));
	ms = discord_helpers_send_backoff_ms(0, 1.5, 44, 400, 300000);
	ASSERT(ms == 1500 + (44 % 50));
	ms = discord_helpers_send_backoff_ms(0, -1.0, 0, 400, 300000);
	ASSERT(ms >= 100);
	ms = discord_helpers_send_backoff_ms(10, -1.0, 999999, 400, 500);
	ASSERT(ms == 500);
	return 0;
}

static int test_lifecycle_str(void)
{
	ASSERT(strcmp(discord_lifecycle_str(DISCORD_LIFECYCLE_CONNECTED), "connected") == 0);
	ASSERT(strcmp(discord_lifecycle_str(DISCORD_LIFECYCLE_DISABLED), "disabled") == 0);
	return 0;
}

static int test_route_message_create_fixture(void)
{
	const char *allowed[] = { "user-42" };
	const char *dm_fixture = "{\"author\":{\"id\":\"user-42\",\"bot\":false},\"channel_id\":\"chan-1\"}";
	const char *guild_no_mention = "{\"author\":{\"id\":\"user-42\",\"bot\":false},"
	                               "\"guild_id\":\"g1\",\"channel_id\":\"chan-2\","
	                               "\"mentions\":[]}";
	const char *guild_with_mention = "{\"author\":{\"id\":\"user-42\",\"bot\":false},"
	                                   "\"guild_id\":\"g1\",\"channel_id\":\"chan-2\","
	                                   "\"mentions\":[{\"id\":\"bot-9\"}]}";
	cJSON *dm = cJSON_Parse(dm_fixture);
	cJSON *g0 = cJSON_Parse(guild_no_mention);
	cJSON *g1 = cJSON_Parse(guild_with_mention);
	char sess[128];
	ASSERT(dm && g0 && g1);
	ASSERT(discord_helpers_route_message_create(dm, allowed, 1, "bot-9", sess, sizeof(sess)) == 1);
	ASSERT(strcmp(sess, "discord:c:chan-1") == 0);
	ASSERT(discord_helpers_route_message_create(dm, allowed, 1, NULL, sess, sizeof(sess)) == 1);
	ASSERT(discord_helpers_route_message_create(dm, allowed, 1, "", sess, sizeof(sess)) == 1);
	ASSERT(discord_helpers_route_message_create(g0, allowed, 1, "bot-9", sess, sizeof(sess)) == 0);
	ASSERT(discord_helpers_route_message_create(g1, allowed, 1, "bot-9", sess, sizeof(sess)) == 1);
	ASSERT(strcmp(sess, "discord:c:chan-2") == 0);
	cJSON_Delete(dm);
	cJSON_Delete(g0);
	cJSON_Delete(g1);
	return 0;
}

/**
 * Negative cases so allowlist/mention gating cannot silently widen:
 * null payload is invalid; bot authors, empty author ids, and guild
 * traffic without bot identity must be ignored (not accepted).
 */
static int test_route_message_create_rejects_edge_cases(void)
{
	const char *allowed[] = { "user-42" };
	const char *bot_author = "{\"author\":{\"id\":\"user-42\",\"bot\":true},\"channel_id\":\"chan-1\"}";
	const char *empty_author_id = "{\"author\":{\"id\":\"\",\"bot\":false},\"channel_id\":\"chan-1\"}";
	const char *guild_empty_bot = "{\"author\":{\"id\":\"user-42\",\"bot\":false},"
	                              "\"guild_id\":\"g1\",\"channel_id\":\"chan-2\","
	                              "\"mentions\":[{\"id\":\"bot-9\"}]}";
	cJSON *bot;
	cJSON *empty_id;
	cJSON *guild;
	char sess[128];

	bot = cJSON_Parse(bot_author);
	empty_id = cJSON_Parse(empty_author_id);
	guild = cJSON_Parse(guild_empty_bot);
	ASSERT(bot && empty_id && guild);
	memset(sess, 'x', sizeof(sess));
	sess[sizeof(sess) - 1] = '\0';
	ASSERT(discord_helpers_route_message_create(NULL, allowed, 1, "bot-9", sess, sizeof(sess)) == -1);
	ASSERT(discord_helpers_route_message_create(bot, allowed, 1, "bot-9", sess, sizeof(sess)) == 0);
	ASSERT(sess[0] == '\0');
	ASSERT(discord_helpers_route_message_create(empty_id, allowed, 1, "bot-9", sess, sizeof(sess)) == 0);
	ASSERT(sess[0] == '\0');
	ASSERT(discord_helpers_route_message_create(guild, allowed, 1, "", sess, sizeof(sess)) == 0);
	ASSERT(sess[0] == '\0');
	ASSERT(discord_helpers_route_message_create(guild, allowed, 1, NULL, sess, sizeof(sess)) == 0);
	ASSERT(sess[0] == '\0');
	cJSON_Delete(bot);
	cJSON_Delete(empty_id);
	cJSON_Delete(guild);
	return 0;
}

/** Exact payload fill of a power-of-two cap must still reserve one byte for NUL. */
static int test_rx_append_grows_for_nul(void)
{
	char *buf;
	size_t len;
	size_t cap;
	char a[4095];
	char one;
	char *big;
	char *big2;

	buf = NULL;
	len = 0;
	cap = 0;
	one = 'Z';
	memset(a, 'A', sizeof(a));
	ASSERT(discord_helpers_rx_append(&buf, &len, &cap, a, sizeof(a), 512 * 1024) == 0);
	ASSERT(len == sizeof(a));
	ASSERT(cap >= len + 1);
	ASSERT(buf[len] == '\0');
	ASSERT(discord_helpers_rx_append(&buf, &len, &cap, &one, 1, 512 * 1024) == 0);
	ASSERT(len == 4096);
	ASSERT(cap >= 4097);
	ASSERT(buf[4096] == '\0');
	ASSERT(buf[4095] == 'Z');

	free(buf);
	buf = NULL;
	len = 0;
	cap = 0;
	big = malloc(65536);
	big2 = malloc(65536);
	ASSERT(big && big2);
	memset(big, 'B', 65536);
	memset(big2, 'C', 65536);
	ASSERT(discord_helpers_rx_append(&buf, &len, &cap, big, 65536, 512 * 1024) == 0);
	ASSERT(len == 65536);
	ASSERT(cap == 131072);
	ASSERT(discord_helpers_rx_append(&buf, &len, &cap, big2, 65536, 512 * 1024) == 0);
	ASSERT(len == 131072);
	ASSERT(cap >= 131073);
	ASSERT(buf[131072] == '\0');
	ASSERT(buf[0] == 'B' && buf[65535] == 'B');
	ASSERT(buf[65536] == 'C' && buf[131071] == 'C');
	free(big);
	free(big2);
	free(buf);
	return 0;
}

static int test_rx_append_rejects_over_max(void)
{
	char *buf;
	size_t len;
	size_t cap;
	char chunk[16];

	buf = NULL;
	len = 0;
	cap = 0;
	memset(chunk, 'x', sizeof(chunk));
	ASSERT(discord_helpers_rx_append(&buf, &len, &cap, chunk, sizeof(chunk), 8) != 0);
	ASSERT(buf == NULL);
	return 0;
}

int main(void)
{
	RUN(test_allow_entry_equals());
	RUN(test_allowlist_array());
	RUN(test_session_id_format());
	RUN(test_mentions_fixture());
	RUN(test_backoff_math());
	RUN(test_lifecycle_str());
	RUN(test_route_message_create_fixture());
	RUN(test_route_message_create_rejects_edge_cases());
	RUN(test_rx_append_grows_for_nul());
	RUN(test_rx_append_rejects_over_max());
	printf("test_discord_helpers: all tests passed\n");
	return 0;
}
