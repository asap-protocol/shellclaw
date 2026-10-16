/**
 * @file test_gateway_db_lock.c
 * @brief Dashboard SQLite routes must hold agent_lock (SQLITE_THREADSAFE=0).
 */
#define _POSIX_C_SOURCE 200809L

#include "core/agent.h"
#include "core/memory.h"
#include "gateway/http_lws.h"
#include "gateway/routes.h"
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
	int status = 0;

	memset(&ctx, 0, sizeof(ctx));
	remove(path);
	ASSERT(memory_init(path) == 0);
	g_sqlite_hits = 0;
	g_sqlite_unlocked = 0;
	memory_sqlite_entry_set_hook_for_test(sqlite_probe);

	ASSERT(call_route(&ctx, HTTP_GET, "/api/sessions", NULL, &status) == 0);
	ASSERT(status == 200);
	ASSERT(call_route(&ctx, HTTP_GET, "/api/memory", NULL, &status) == 0);
	ASSERT(call_route(&ctx, HTTP_GET, "/api/cron", NULL, &status) == 0);
	ASSERT(status == 200);
	ASSERT(call_route(&ctx, HTTP_POST, "/api/cron", create_body, &status) == 0);
	ASSERT(status == 201);
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

int main(void)
{
	if (test_dashboard_sqlite_holds_agent_lock() != 0)
		return 1;
	printf("test_gateway_db_lock: all tests passed\n");
	return 0;
}
