/**
 * @file test_gateway_db_lock.c
 * @brief Dashboard SQLite routes must hold agent_lock (SQLITE_THREADSAFE=0).
 */
#define _POSIX_C_SOURCE 200809L

#include "channels/channel.h"
#include "channels/webchat.h"
#include "core/agent.h"
#include "core/dispatch.h"
#include "core/memory.h"
#include "gateway/http_lws.h"
#include "gateway/routes.h"
#include "sqlite3.h"
#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>

#define ASSERT(c) do { \
	if (!(c)) { \
		fprintf(stderr, "FAIL: %s:%d %s\n", __FILE__, __LINE__, #c); \
		return 1; \
	} \
} while (0)

static int g_sqlite_hits;
static int g_sqlite_unlocked;

static void sqlite_probe(void)
{
	g_sqlite_hits++;
	if (!agent_mutex_is_locked_for_test())
		g_sqlite_unlocked = 1;
}

static int call_route(http_server_ctx_t *ctx, int method, const char *uri,
		      const char *body, int *status)
{
	char buf[4096];
	int hits_before = g_sqlite_hits;

	buf[0] = '\0';
	*status = 0;
	ASSERT(dispatch_route(ctx, NULL, method, uri, (int)strlen(uri),
			      body, body ? strlen(body) : 0, buf, sizeof(buf), status) == 0);
	ASSERT(g_sqlite_hits > hits_before);
	ASSERT(g_sqlite_unlocked == 0);
	ASSERT(agent_mutex_is_locked_for_test() == 0);
	return 0;
}

static int test_dashboard_sqlite_holds_agent_lock(void)
{
	http_server_ctx_t ctx;
	const char *path = "/tmp/shellclaw_test_gateway_db_lock.db";
	const char *create_body =
		"{\"id\":\"job_lock\",\"schedule\":\"interval:3600\",\"message\":\"ping\"}";
	const char *invalid_body =
		"{\"schedule\":\"interval:3600\",\"message\":\"ping\","
		"\"channel\":\"webchat\",\"recipient\":\"42\"}";
	char response[128];
	int status = 0;

	memset(&ctx, 0, sizeof(ctx));
	remove(path);
	ASSERT(memory_init(path) == 0);
	channel_register("cli", channel_cli_get());
	channel_register("webchat", channel_webchat_get());
	g_sqlite_hits = 0;
	g_sqlite_unlocked = 0;
	memory_sqlite_entry_set_hook_for_test(sqlite_probe);

	ASSERT(call_route(&ctx, HTTP_GET, "/api/sessions", NULL, &status) == 0);
	ASSERT(status == 200);
	ASSERT(session_save("sess_lock", "[]") == 0);
	ASSERT(call_route(&ctx, HTTP_DELETE, "/api/sessions/sess_lock", NULL, &status) == 0);
	ASSERT(status == 200);
	ASSERT(call_route(&ctx, HTTP_GET, "/api/memory", NULL, &status) == 0);
	ASSERT(call_route(&ctx, HTTP_GET, "/api/cron", NULL, &status) == 0);
	ASSERT(status == 200);
	ASSERT(call_route(&ctx, HTTP_POST, "/api/cron", create_body, &status) == 0);
	ASSERT(status == 201);
	ASSERT(dispatch_route(&ctx, NULL, HTTP_POST, "/api/cron", 9, invalid_body,
			      strlen(invalid_body), response, sizeof(response), &status) == 0);
	ASSERT(status == 400);
	ASSERT(strstr(response, "Invalid cron destination") != NULL);
	ASSERT(call_route(&ctx, HTTP_POST, "/api/cron/job_lock/toggle", NULL, &status) == 0);
	ASSERT(status == 200);
	ASSERT(call_route(&ctx, HTTP_DELETE, "/api/cron/job_lock", NULL, &status) == 0);
	ASSERT(status == 200);

	memory_sqlite_entry_set_hook_for_test(NULL);
	ASSERT(agent_mutex_is_locked_for_test() == 0);
	memory_cleanup();
	remove(path);
	return 0;
}

static int test_webchat_retries_reply(void)
{
	char path[] = "/tmp/shellclaw_test_webchat_retry_XXXXXX";
	const char *session = WEBCHAT_DEVICE_PREFIX "test-device";
	const char *reject_reply = "CREATE TRIGGER reject_chat_reply BEFORE UPDATE ON webchat_turns "
		"WHEN new.state=2 BEGIN SELECT RAISE(ABORT,'Test failed reply write'); END;";
	sqlite3 *maintenance = NULL;
	channel_incoming_msg_t msg = {0};
	char history[2048];
	const channel_t *chat = channel_webchat_get();
	int fd = mkstemp(path);
	ASSERT(fd >= 0);
	close(fd);
	ASSERT(memory_init(path) == 0);
	ASSERT(chat->init(NULL) == 0);
	ASSERT(webchat_store_accept(session, "first", "/status") == 0);
	ASSERT(webchat_store_accept(session, "second", "/reset") == 0);
	ASSERT(chat->poll(&msg, 0) == 1);
	ASSERT(strcmp(msg.text, "/status") == 0);
	ASSERT(sqlite3_open(path, &maintenance) == SQLITE_OK);
	ASSERT(sqlite3_exec(maintenance, reject_reply, NULL, NULL, NULL) == SQLITE_OK);
	(void)handle_message(chat, &msg);
	channel_incoming_msg_clear(&msg);
	ASSERT(webchat_store_history(session, 0, history, sizeof(history)) == 0);
	ASSERT(strstr(history, "\"reply\":null") != NULL);
	ASSERT(!strstr(history, "second"));
	ASSERT(sqlite3_exec(maintenance, "DROP TRIGGER reject_chat_reply", NULL, NULL, NULL) == SQLITE_OK);

	ASSERT(chat->poll(&msg, 0) == 1);
	ASSERT(strcmp(msg.text, "/reset") == 0);
	ASSERT(webchat_store_history(session, 0, history, sizeof(history)) == 0);
	int saved_before_next = strstr(history, "\"id\":\"first\",\"text\":\"/status\",\"reply\":\"ShellClaw ") != NULL;
	ASSERT(sqlite3_exec(maintenance, reject_reply, NULL, NULL, NULL) == SQLITE_OK);
	(void)handle_message(chat, &msg);
	channel_incoming_msg_clear(&msg);
	ASSERT(sqlite3_exec(maintenance, "DROP TRIGGER reject_chat_reply", NULL, NULL, NULL) == SQLITE_OK);
	ASSERT(sqlite3_close(maintenance) == SQLITE_OK);
	chat->cleanup();

	ASSERT(webchat_store_recover() == 0);
	ASSERT(webchat_store_history(session, 0, history, sizeof(history)) == 0);
	int separate_replies = strstr(history, "\"id\":\"first\",\"text\":\"/status\",\"reply\":\"ShellClaw ") &&
		strstr(history, "\"id\":\"second\",\"text\":\"/reset\",\"reply\":\"Model context cleared.");
	ASSERT(webchat_store_delete(session) == 0);
	memory_cleanup();
	remove(path);
	ASSERT(saved_before_next && separate_replies);
	return 0;
}

int main(void)
{
	int failed = test_dashboard_sqlite_holds_agent_lock();
	failed |= test_webchat_retries_reply();
	if (failed) return 1;
	printf("test_gateway_db_lock: all tests passed\n");
	return 0;
}
