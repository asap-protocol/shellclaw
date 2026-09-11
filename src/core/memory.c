/**
 * @file memory.c
 * @brief SQLite memory store: schema (memories + FTS5, sessions), save/recall, session CRUD.
 */
#define _POSIX_C_SOURCE 200809L

#include "core/memory.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "sqlite3.h"

#define WARN_RECREATED "Warning: memory DB invalid or corrupted, recreated at %s\n"

static sqlite3 *g_db;

static const char *SCHEMA_MEMORIES =
	"CREATE TABLE IF NOT EXISTS memories ("
	"  rowid INTEGER PRIMARY KEY,"
	"  key TEXT UNIQUE NOT NULL,"
	"  content TEXT NOT NULL,"
	"  metadata TEXT,"
	"  created_at TEXT,"
	"  updated_at TEXT"
	");";

static const char *SCHEMA_MEMORIES_FTS =
	"CREATE VIRTUAL TABLE IF NOT EXISTS memories_fts USING fts5("
	"  content,"
	"  key,"
	"  content='memories',"
	"  content_rowid='rowid'"
	");";

static const char *TRIGGER_MEMORIES_AI =
	"CREATE TRIGGER IF NOT EXISTS memories_ai AFTER INSERT ON memories BEGIN "
	"  INSERT INTO memories_fts(rowid, content, key) VALUES (new.rowid, new.content, new.key); "
	"END;";

static const char *TRIGGER_MEMORIES_AD =
	"CREATE TRIGGER IF NOT EXISTS memories_ad AFTER DELETE ON memories BEGIN "
	"  INSERT INTO memories_fts(memories_fts, rowid, content, key) VALUES ('delete', old.rowid, old.content, old.key); "
	"END;";

static const char *TRIGGER_MEMORIES_AU =
	"CREATE TRIGGER IF NOT EXISTS memories_au AFTER UPDATE ON memories BEGIN "
	"  INSERT INTO memories_fts(memories_fts, rowid, content, key) VALUES ('delete', old.rowid, old.content, old.key); "
	"  INSERT INTO memories_fts(rowid, content, key) VALUES (new.rowid, new.content, new.key); "
	"END;";

static const char *SCHEMA_SESSIONS =
	"CREATE TABLE IF NOT EXISTS sessions ("
	"  id TEXT PRIMARY KEY,"
	"  messages TEXT NOT NULL DEFAULT '[]',"
	"  created_at TEXT,"
	"  updated_at TEXT"
	");";

static const char *SCHEMA_CRON_JOBS =
	"CREATE TABLE IF NOT EXISTS cron_jobs ("
	"  id TEXT PRIMARY KEY,"
	"  schedule TEXT NOT NULL,"
	"  message TEXT NOT NULL,"
	"  channel TEXT,"
	"  recipient TEXT,"
	"  next_run INTEGER NOT NULL,"
	"  enabled INTEGER NOT NULL DEFAULT 1"
	");";

static const char *SCHEMA_CONFIG_KV =
	"CREATE TABLE IF NOT EXISTS config_kv ("
	"  key TEXT PRIMARY KEY,"
	"  value TEXT NOT NULL"
	");";

#define SCHEMA_VERSION "2"

static int run_schema(void)
{
	char *err = NULL;
	if (sqlite3_exec(g_db, SCHEMA_MEMORIES, NULL, NULL, &err) != SQLITE_OK) goto fail;
	if (sqlite3_exec(g_db, SCHEMA_MEMORIES_FTS, NULL, NULL, &err) != SQLITE_OK) goto fail;
	if (sqlite3_exec(g_db, TRIGGER_MEMORIES_AI, NULL, NULL, &err) != SQLITE_OK) goto fail;
	if (sqlite3_exec(g_db, TRIGGER_MEMORIES_AD, NULL, NULL, &err) != SQLITE_OK) goto fail;
	if (sqlite3_exec(g_db, TRIGGER_MEMORIES_AU, NULL, NULL, &err) != SQLITE_OK) goto fail;
	if (sqlite3_exec(g_db, SCHEMA_SESSIONS, NULL, NULL, &err) != SQLITE_OK) goto fail;
	if (sqlite3_exec(g_db, SCHEMA_CRON_JOBS, NULL, NULL, &err) != SQLITE_OK) goto fail;
	if (sqlite3_exec(g_db, SCHEMA_CONFIG_KV, NULL, NULL, &err) != SQLITE_OK) goto fail;
	return 0;
fail:
	if (err) sqlite3_free(err);
	return -1;
}

static int run_migration(void)
{
	const char *sql = "INSERT OR REPLACE INTO config_kv(key, value) VALUES('schema_version', ?1)";
	sqlite3_stmt *stmt = NULL;
	if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) != SQLITE_OK) return -1;
	sqlite3_bind_text(stmt, 1, SCHEMA_VERSION, -1, SQLITE_TRANSIENT);
	int ret = sqlite3_step(stmt) == SQLITE_DONE ? 0 : -1;
	sqlite3_finalize(stmt);
	if (ret != 0) return ret;
	return 0;
}

static int path_exists(const char *path)
{
	struct stat st;
	return stat(path, &st) == 0;
}

int memory_init(const char *path)
{
	if (!path || g_db) return -1;
	int file_existed = path_exists(path);
	int recreated = 0;
	if (sqlite3_open(path, &g_db) != SQLITE_OK) {
		if (g_db) { sqlite3_close(g_db); g_db = NULL; }
		remove(path);
		if (sqlite3_open(path, &g_db) != SQLITE_OK) {
			if (g_db) sqlite3_close(g_db);
			g_db = NULL;
			return -1;
		}
		recreated = 1;
	}
	sqlite3_exec(g_db, "PRAGMA journal_mode=WAL;", NULL, NULL, NULL);
	sqlite3_busy_timeout(g_db, 5000);
	if (run_schema() != 0 || run_migration() != 0) {
		if (file_existed) {
			fprintf(stderr, "Error: memory DB schema mismatch at %s\n", path);
			sqlite3_close(g_db);
			g_db = NULL;
			return -1;
		}
		sqlite3_close(g_db);
		g_db = NULL;
		remove(path);
		if (sqlite3_open(path, &g_db) != SQLITE_OK) {
			if (g_db) sqlite3_close(g_db);
			g_db = NULL;
			return -1;
		}
		sqlite3_exec(g_db, "PRAGMA journal_mode=WAL;", NULL, NULL, NULL);
		sqlite3_busy_timeout(g_db, 5000);
		if (run_schema() != 0 || run_migration() != 0) {
			sqlite3_close(g_db);
			g_db = NULL;
			return -1;
		}
		recreated = 1;
	}
	/* FTS5 integrity-check; step result ignored. */
	sqlite3_stmt *stmt = NULL;
	if (sqlite3_prepare_v2(g_db, "INSERT INTO memories_fts(memories_fts) VALUES('integrity-check')", -1, &stmt, NULL) == SQLITE_OK) {
		(void)sqlite3_step(stmt);
		sqlite3_finalize(stmt);
	}
	if (recreated) fprintf(stderr, WARN_RECREATED, path);
	return 0;
}

int memory_save(const char *key, const char *content, const char *metadata)
{
	if (!g_db || !key || !content) return -1;
	const char *meta = metadata ? metadata : "";
	const char *sql = "INSERT INTO memories(key, content, metadata, created_at, updated_at) "
		"VALUES(?1, ?2, ?3, datetime('now'), datetime('now')) "
		"ON CONFLICT(key) DO UPDATE SET content=excluded.content, metadata=excluded.metadata, updated_at=datetime('now')";
	sqlite3_stmt *stmt = NULL;
	if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) != SQLITE_OK) return -1;
	sqlite3_bind_text(stmt, 1, key, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 2, content, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 3, meta, -1, SQLITE_TRANSIENT);
	int ret = sqlite3_step(stmt) == SQLITE_DONE ? 0 : -1;
	sqlite3_finalize(stmt);
	return ret;
}

int memory_recall(const char *query, char *results, size_t max_len, int limit)
{
	if (!g_db || !results || max_len == 0 || !query) return -1;
	results[0] = '\0';
	if (limit <= 0) limit = 10;
	const char *sql = "SELECT content FROM memories_fts WHERE memories_fts MATCH ?1 ORDER BY rank LIMIT ?2";
	sqlite3_stmt *stmt = NULL;
	if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) != SQLITE_OK) return -1;
	sqlite3_bind_text(stmt, 1, query, -1, SQLITE_TRANSIENT);
	sqlite3_bind_int(stmt, 2, limit);
	size_t len = 0;
	while (sqlite3_step(stmt) == SQLITE_ROW && len < max_len - 1) {
		const char *content = (const char *)sqlite3_column_text(stmt, 0);
		if (content) {
			if (len > 0) {
				if (len + 2 >= max_len) break;
				results[len++] = '\n';
				results[len++] = '\n';
			}
			{
				size_t n = strlen(content);
				size_t remain = max_len - len - 1;
				if (n > remain) n = remain;
				memcpy(results + len, content, n);
				len += n;
				results[len] = '\0';
			}
		}
	}
	sqlite3_finalize(stmt);
	return 0;
}

int session_load(const char *session_id, char *messages_out, size_t max_len)
{
	if (!g_db || !session_id || !messages_out || max_len == 0) return -1;
	messages_out[0] = '\0';
	const char *sql = "SELECT messages FROM sessions WHERE id = ?1";
	sqlite3_stmt *stmt = NULL;
	if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) != SQLITE_OK) return -1;
	sqlite3_bind_text(stmt, 1, session_id, -1, SQLITE_TRANSIENT);
	int ret = -1;
	if (sqlite3_step(stmt) == SQLITE_ROW) {
		const char *msg = (const char *)sqlite3_column_text(stmt, 0);
		if (msg) {
			size_t n = strlen(msg);
			if (n >= max_len) n = max_len - 1;
			memcpy(messages_out, msg, n);
			messages_out[n] = '\0';
			ret = 0;
		}
	}
	sqlite3_finalize(stmt);
	return ret;
}

int session_save(const char *session_id, const char *messages)
{
	if (!g_db || !session_id || !messages) return -1;
	const char *sql = "INSERT INTO sessions(id, messages, created_at, updated_at) "
		"VALUES(?1, ?2, datetime('now'), datetime('now')) "
		"ON CONFLICT(id) DO UPDATE SET messages=excluded.messages, updated_at=datetime('now')";
	sqlite3_stmt *stmt = NULL;
	if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) != SQLITE_OK) return -1;
	sqlite3_bind_text(stmt, 1, session_id, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 2, messages, -1, SQLITE_TRANSIENT);
	int ret = sqlite3_step(stmt) == SQLITE_DONE ? 0 : -1;
	sqlite3_finalize(stmt);
	return ret;
}

int session_delete(const char *session_id)
{
	if (!g_db || !session_id) return -1;
	const char *sql = "DELETE FROM sessions WHERE id = ?1";
	sqlite3_stmt *stmt = NULL;
	if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) != SQLITE_OK) return -1;
	sqlite3_bind_text(stmt, 1, session_id, -1, SQLITE_TRANSIENT);
	int ret = (sqlite3_step(stmt) == SQLITE_DONE && sqlite3_changes(g_db) > 0) ? 0 : -1;
	sqlite3_finalize(stmt);
	return ret;
}

int session_list(char **session_ids_out, int max_count)
{
	if (!g_db || !session_ids_out || max_count <= 0) return -1;
	const char *sql = "SELECT id FROM sessions ORDER BY updated_at DESC";
	sqlite3_stmt *stmt = NULL;
	if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) != SQLITE_OK) return -1;
	int count = 0;
	while (count < max_count && sqlite3_step(stmt) == SQLITE_ROW) {
		const char *id = (const char *)sqlite3_column_text(stmt, 0);
		if (id) {
			session_ids_out[count] = strdup(id);
			if (!session_ids_out[count]) {
				for (int i = 0; i < count; i++) free(session_ids_out[i]);
				sqlite3_finalize(stmt);
				return -1;
			}
			count++;
		}
	}
	sqlite3_finalize(stmt);
	return count;
}

int config_kv_get(const char *key, char *value_out, size_t max_len)
{
	if (!g_db || !key || !value_out || max_len == 0) return -1;
	value_out[0] = '\0';
	const char *sql = "SELECT value FROM config_kv WHERE key = ?1";
	sqlite3_stmt *stmt = NULL;
	if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) != SQLITE_OK) return -1;
	sqlite3_bind_text(stmt, 1, key, -1, SQLITE_TRANSIENT);
	int ret = -1;
	if (sqlite3_step(stmt) == SQLITE_ROW) {
		const char *val = (const char *)sqlite3_column_text(stmt, 0);
		if (val) {
			size_t n = strlen(val);
			if (n >= max_len) n = max_len - 1;
			memcpy(value_out, val, n);
			value_out[n] = '\0';
			ret = 0;
		}
	}
	sqlite3_finalize(stmt);
	return ret;
}

int config_kv_set(const char *key, const char *value)
{
	if (!g_db || !key || !value) return -1;
	const char *sql = "INSERT INTO config_kv(key, value) VALUES(?1, ?2) "
		"ON CONFLICT(key) DO UPDATE SET value=excluded.value";
	sqlite3_stmt *stmt = NULL;
	if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) != SQLITE_OK) return -1;
	sqlite3_bind_text(stmt, 1, key, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 2, value, -1, SQLITE_TRANSIENT);
	int ret = sqlite3_step(stmt) == SQLITE_DONE ? 0 : -1;
	sqlite3_finalize(stmt);
	return ret;
}

static void copy_str_bounded(char *dst, size_t dst_size, const char *src)
{
	if (!dst || dst_size == 0) return;
	if (!src) { dst[0] = '\0'; return; }
	size_t n = strlen(src);
	if (n >= dst_size) n = dst_size - 1;
	memcpy(dst, src, n);
	dst[n] = '\0';
}

static char *dup_sqlite_text(sqlite3_stmt *stmt, int col)
{
	const char *src = (const char *)sqlite3_column_text(stmt, col);
	return strdup(src ? src : "");
}

void cron_job_row_free(cron_job_row_t *row)
{
	if (!row) return;
	free(row->schedule);
	free(row->message);
	row->schedule = NULL;
	row->message = NULL;
}

static int fill_cron_job_row(sqlite3_stmt *stmt, cron_job_row_t *out)
{
	char *schedule = dup_sqlite_text(stmt, 1);
	char *message = dup_sqlite_text(stmt, 2);
	if (!schedule || !message) {
		free(schedule);
		free(message);
		return -1;
	}
	copy_str_bounded(out->id, sizeof(out->id), (const char *)sqlite3_column_text(stmt, 0));
	out->schedule = schedule;
	out->message = message;
	copy_str_bounded(out->channel, sizeof(out->channel), (const char *)sqlite3_column_text(stmt, 3));
	copy_str_bounded(out->recipient, sizeof(out->recipient), (const char *)sqlite3_column_text(stmt, 4));
	out->next_run = sqlite3_column_int64(stmt, 5);
	out->enabled = sqlite3_column_int(stmt, 6);
	return 0;
}

int cron_job_create(const char *id, const char *schedule, const char *message,
                    const char *channel, const char *recipient, long long next_run, int enabled)
{
	if (!g_db || !id || !schedule || !message) return -1;
	const char *ch = channel ? channel : "";
	const char *rec = recipient ? recipient : "";
	const char *sql = "INSERT INTO cron_jobs(id, schedule, message, channel, recipient, next_run, enabled) "
		"VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7)";
	sqlite3_stmt *stmt = NULL;
	if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) != SQLITE_OK) return -1;
	sqlite3_bind_text(stmt, 1, id, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 2, schedule, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 3, message, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 4, ch, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 5, rec, -1, SQLITE_TRANSIENT);
	sqlite3_bind_int64(stmt, 6, next_run);
	sqlite3_bind_int(stmt, 7, enabled ? 1 : 0);
	int ret = sqlite3_step(stmt) == SQLITE_DONE ? 0 : -1;
	sqlite3_finalize(stmt);
	return ret;
}

int cron_job_delete(const char *id)
{
	if (!g_db || !id) return -1;
	const char *sql = "DELETE FROM cron_jobs WHERE id = ?1";
	sqlite3_stmt *stmt = NULL;
	if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) != SQLITE_OK) return -1;
	sqlite3_bind_text(stmt, 1, id, -1, SQLITE_TRANSIENT);
	int ret = (sqlite3_step(stmt) == SQLITE_DONE && sqlite3_changes(g_db) > 0) ? 0 : -1;
	sqlite3_finalize(stmt);
	return ret;
}

int cron_job_toggle(const char *id)
{
	if (!g_db || !id) return -1;
	const char *sql = "UPDATE cron_jobs SET enabled = 1 - enabled WHERE id = ?1";
	sqlite3_stmt *stmt = NULL;
	if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) != SQLITE_OK) return -1;
	sqlite3_bind_text(stmt, 1, id, -1, SQLITE_TRANSIENT);
	int ret = (sqlite3_step(stmt) == SQLITE_DONE && sqlite3_changes(g_db) > 0) ? 0 : -1;
	sqlite3_finalize(stmt);
	return ret;
}

int cron_job_update_next_run(const char *id, long long next_run)
{
	if (!g_db || !id) return -1;
	const char *sql = "UPDATE cron_jobs SET next_run = ?1 WHERE id = ?2";
	sqlite3_stmt *stmt = NULL;
	if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) != SQLITE_OK) return -1;
	sqlite3_bind_int64(stmt, 1, next_run);
	sqlite3_bind_text(stmt, 2, id, -1, SQLITE_TRANSIENT);
	int ret = sqlite3_step(stmt) == SQLITE_DONE ? 0 : -1;
	sqlite3_finalize(stmt);
	return ret;
}

int cron_job_list(cron_job_row_t *out, int max_count)
{
	if (!g_db || !out || max_count <= 0) return -1;
	const char *sql = "SELECT id, schedule, message, channel, recipient, next_run, enabled FROM cron_jobs ORDER BY next_run ASC";
	sqlite3_stmt *stmt = NULL;
	if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) != SQLITE_OK) return -1;
	int count = 0;
	while (count < max_count && sqlite3_step(stmt) == SQLITE_ROW) {
		if (fill_cron_job_row(stmt, &out[count]) != 0) {
			for (int i = 0; i < count; i++)
				cron_job_row_free(&out[i]);
			sqlite3_finalize(stmt);
			return -1;
		}
		count++;
	}
	sqlite3_finalize(stmt);
	return count;
}

int cron_job_get_next_due(long long now, cron_job_row_t *out)
{
	if (!g_db || !out) return -1;
	const char *sql = "SELECT id, schedule, message, channel, recipient, next_run, enabled FROM cron_jobs "
		"WHERE next_run <= ?1 AND enabled = 1 ORDER BY next_run ASC LIMIT 1";
	sqlite3_stmt *stmt = NULL;
	if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) != SQLITE_OK) return -1;
	sqlite3_bind_int64(stmt, 1, now);
	int ret = 0;
	if (sqlite3_step(stmt) == SQLITE_ROW) {
		if (fill_cron_job_row(stmt, out) != 0) {
			sqlite3_finalize(stmt);
			return -1;
		}
		ret = 1;
	}
	sqlite3_finalize(stmt);
	return ret;
}

void memory_cleanup(void)
{
	if (g_db) {
		sqlite3_close(g_db);
		g_db = NULL;
	}
}

static int count_table(const char *sql, int *out_count)
{
	sqlite3_stmt *stmt = NULL;
	if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) != SQLITE_OK) return -1;
	if (sqlite3_step(stmt) != SQLITE_ROW) {
		sqlite3_finalize(stmt);
		return -1;
	}
	*out_count = sqlite3_column_int(stmt, 0);
	sqlite3_finalize(stmt);
	return 0;
}

int memory_get_row_counts(int *sessions_out, int *memories_out, int *cron_jobs_out)
{
	if (!g_db) return -1;
	if (sessions_out) {
		if (count_table("SELECT COUNT(*) FROM sessions", sessions_out) != 0) return -1;
	}
	if (memories_out) {
		if (count_table("SELECT COUNT(*) FROM memories", memories_out) != 0) return -1;
	}
	if (cron_jobs_out) {
		if (count_table("SELECT COUNT(*) FROM cron_jobs", cron_jobs_out) != 0) return -1;
	}
	return 0;
}
