/**
 * @file test_memory.c
 * @brief Unit tests for memory store: schema, save/recall FTS5, session CRUD.
 */

#include "core/memory.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "sqlite3.h"

#define ASSERT(c) do { if (!(c)) { fprintf(stderr, "FAIL: %s:%d %s\n", __FILE__, __LINE__, #c); return 1; } } while (0)
#define RUN(t) do { int r = (t); if (r) return r; } while (0)

static int test_schema_and_fts5(void)
{
	const char *path = "/tmp/shellclaw_test_memory.db";
	remove(path);
	ASSERT(memory_init(path) == 0);
	ASSERT(memory_save("key1", "hello world and FTS5", NULL) == 0);
	ASSERT(memory_save("key2", "foo bar baz", NULL) == 0);
	char buf[512];
	ASSERT(memory_recall("hello", buf, sizeof(buf), 5) == 0);
	ASSERT(strstr(buf, "hello world") != NULL);
	ASSERT(memory_recall("bar", buf, sizeof(buf), 5) == 0);
	ASSERT(strstr(buf, "foo bar") != NULL);
	memory_cleanup();
	remove(path);
	return 0;
}

static int test_save_overwrite(void)
{
	const char *path = "/tmp/shellclaw_test_memory2.db";
	remove(path);
	ASSERT(memory_init(path) == 0);
	ASSERT(memory_save("k", "first", NULL) == 0);
	ASSERT(memory_save("k", "second", NULL) == 0);
	char buf[256];
	ASSERT(memory_recall("second", buf, sizeof(buf), 5) == 0);
	ASSERT(strstr(buf, "second") != NULL);
	memory_cleanup();
	remove(path);
	return 0;
}

static int test_corrupted_db_recreated(void)
{
	const char *path = "/tmp/shellclaw_test_corrupt.db";
	remove(path);
	FILE *f = fopen(path, "w");
	ASSERT(f);
	fprintf(f, "not a sqlite database\n");
	fclose(f);
	/* With CR-18: existing file with invalid content causes schema failure; we do not delete. */
	ASSERT(memory_init(path) == -1);
	remove(path);
	/* Fresh file: init succeeds and we can use the DB. */
	ASSERT(memory_init(path) == 0);
	ASSERT(memory_save("k", "after recreate", NULL) == 0);
	char buf[256];
	ASSERT(memory_recall("recreate", buf, sizeof(buf), 5) == 0);
	ASSERT(strstr(buf, "after recreate") != NULL);
	memory_cleanup();
	remove(path);
	return 0;
}

static int test_session_list(void)
{
	const char *path = "/tmp/shellclaw_test_memory_list.db";
	remove(path);
	ASSERT(memory_init(path) == 0);
	ASSERT(session_save("s1", "[]") == 0);
	ASSERT(session_save("s2", "[]") == 0);
	ASSERT(session_save("s3", "[]") == 0);
	char *ids[8];
	int n = session_list(ids, 8);
	ASSERT(n >= 3);
	for (int i = 0; i < n; i++)
		free(ids[i]);
	n = session_list(ids, 2);
	ASSERT(n == 2);
	for (int i = 0; i < n; i++)
		free(ids[i]);
	memory_cleanup();
	remove(path);
	return 0;
}

static int test_session_crud(void)
{
	const char *path = "/tmp/shellclaw_test_memory3.db";
	remove(path);
	ASSERT(memory_init(path) == 0);
	const char *sid = "cli:default";
	const char *messages = "[{\"role\":\"user\",\"content\":\"hi\"},{\"role\":\"assistant\",\"content\":\"hello\"}]";
	ASSERT(session_save(sid, messages) == 0);
	char loaded[1024];
	ASSERT(session_load(sid, loaded, sizeof(loaded)) == 0);
	ASSERT(strcmp(loaded, messages) == 0);
	ASSERT(session_delete(sid) == 0);
	ASSERT(session_load(sid, loaded, sizeof(loaded)) == -1);
	ASSERT(loaded[0] == '\0');
	memory_cleanup();
	remove(path);
	return 0;
}

static int test_gateway_schema_new_db(void)
{
	const char *path = "/tmp/shellclaw_test_gateway_schema.db";
	remove(path);
	ASSERT(memory_init(path) == 0);
	char schema_ver[32];
	ASSERT(config_kv_get("schema_version", schema_ver, sizeof(schema_ver)) == 0);
	ASSERT(strcmp(schema_ver, "2") == 0);
	ASSERT(config_kv_set("test_key", "test_value") == 0);
	char val[64];
	ASSERT(config_kv_get("test_key", val, sizeof(val)) == 0);
	ASSERT(strcmp(val, "test_value") == 0);
	memory_cleanup();
	remove(path);
	return 0;
}

static int test_gateway_schema_migration_v01(void)
{
	const char *path = "/tmp/shellclaw_test_gateway_migration.db";
	remove(path);
	ASSERT(memory_init(path) == 0);
	ASSERT(memory_save("pre_migration", "data before migration", NULL) == 0);
	ASSERT(session_save("sess:1", "[{\"role\":\"user\",\"content\":\"hi\"}]") == 0);
	memory_cleanup();
	sqlite3 *db = NULL;
	if (sqlite3_open(path, &db) != SQLITE_OK) return 1;
	sqlite3_exec(db, "DROP TABLE IF EXISTS cron_jobs", NULL, NULL, NULL);
	sqlite3_exec(db, "DROP TABLE IF EXISTS config_kv", NULL, NULL, NULL);
	sqlite3_close(db);
	ASSERT(memory_init(path) == 0);
	char schema_ver[32];
	ASSERT(config_kv_get("schema_version", schema_ver, sizeof(schema_ver)) == 0);
	ASSERT(strcmp(schema_ver, "2") == 0);
	char buf[256];
	ASSERT(memory_recall("pre_migration", buf, sizeof(buf), 5) == 0);
	ASSERT(strstr(buf, "data before migration") != NULL);
	char loaded[256];
	ASSERT(session_load("sess:1", loaded, sizeof(loaded)) == 0);
	ASSERT(strstr(loaded, "hi") != NULL);
	memory_cleanup();
	remove(path);
	return 0;
}

static int test_existing_db_preserved_on_open_failure(void)
{
	const char *path = "/tmp/shellclaw_test_open_fail.db";
	remove(path);
	ASSERT(memory_init(path) == 0);
	ASSERT(memory_save("preserve", "important data", NULL) == 0);
	memory_cleanup();
	ASSERT(chmod(path, 0000) == 0);
	ASSERT(memory_init(path) == -1);
	ASSERT(chmod(path, 0600) == 0);
	ASSERT(memory_init(path) == 0);
	char buf[256];
	ASSERT(memory_recall("important", buf, sizeof(buf), 5) == 0);
	ASSERT(strstr(buf, "important data") != NULL);
	memory_cleanup();
	remove(path);
	return 0;
}

int main(void)
{
	RUN(test_schema_and_fts5());
	RUN(test_save_overwrite());
	RUN(test_corrupted_db_recreated());
	RUN(test_existing_db_preserved_on_open_failure());
	RUN(test_session_list());
	RUN(test_session_crud());
	RUN(test_gateway_schema_new_db());
	RUN(test_gateway_schema_migration_v01());
	printf("test_memory: all tests passed\n");
	return 0;
}
