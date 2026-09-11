/**
 * @file memory.h
 * @brief SQLite-backed memory store: long-term memories (FTS5) and session history.
 */

#ifndef SHELLCLAW_MEMORY_H
#define SHELLCLAW_MEMORY_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialize memory store at the given DB path. Creates file and schema if missing.
 *
 * @param path Path to SQLite database (e.g. ~/.shellclaw/memory.db); caller must expand ~.
 * @return 0 on success, non-zero on error.
 */
int memory_init(const char *path);

/**
 * Save or update a long-term memory by key.
 *
 * @param key     Unique key for the memory.
 * @param content Text content (stored and indexed for FTS5).
 * @param metadata Optional JSON metadata (may be NULL).
 * @return 0 on success, non-zero on error.
 */
int memory_save(const char *key, const char *content, const char *metadata);

/**
 * Recall memories matching the query using FTS5 (BM25). Writes up to max_len bytes
 * into results; multiple memories are formatted as newline-separated or JSON array.
 *
 * @param query   FTS5 search query.
 * @param results Output buffer.
 * @param max_len Size of results buffer.
 * @param limit   Maximum number of memories to return.
 * @return 0 on success, non-zero on error.
 */
int memory_recall(const char *query, char *results, size_t max_len, int limit);

/**
 * Load session messages by session ID (e.g. "cli:default" or "telegram:123456789").
 *
 * @param session_id   Session identifier.
 * @param messages_out Output buffer for JSON array of messages; caller must free if allocated.
 * @param max_len      Size of messages_out buffer (or 0 if messages_out is to be allocated by implementation).
 * @return 0 on success, non-zero if not found or error.
 */
int session_load(const char *session_id, char *messages_out, size_t max_len);

/**
 * Save (upsert) session messages by session ID.
 *
 * @param session_id Session identifier.
 * @param messages   JSON array of messages.
 * @return 0 on success, non-zero on error.
 */
int session_save(const char *session_id, const char *messages);

/**
 * Delete a session by ID.
 *
 * @param session_id Session identifier.
 * @return 0 on success, non-zero on error.
 */
int session_delete(const char *session_id);

/**
 * List session IDs from the database.
 *
 * @param session_ids_out Array of pointers to receive session IDs; caller must free each.
 * @param max_count       Maximum number of session IDs to return.
 * @return Number of sessions returned, or -1 on error.
 */
int session_list(char **session_ids_out, int max_count);

/**
 * Get a value from config_kv by key.
 *
 * @param key      Key to look up.
 * @param value_out Output buffer for the value.
 * @param max_len   Size of value_out buffer.
 * @return 0 on success, -1 if not found or error.
 */
int config_kv_get(const char *key, char *value_out, size_t max_len);

/**
 * Set (upsert) a value in config_kv.
 *
 * @param key   Key.
 * @param value Value to store (must not be NULL).
 * @return 0 on success, non-zero on error.
 */
int config_kv_set(const char *key, const char *value);

/**
 * Row from cron_jobs table for list/get operations.
 *
 * schedule and message are heap copies of the SQLite TEXT columns. The
 * caller must cron_job_row_free() each filled row. Example:
 *   cron_job_row_t row;
 *   memset(&row, 0, sizeof(row));
 *   if (cron_job_get_next_due(now, &row) == 1) {
 *       cron_job_row_free(&row);
 *   }
 */
typedef struct cron_job_row {
	char id[128];
	char *schedule;
	char *message;
	char channel[64];
	char recipient[64];
	long long next_run;
	int enabled;
} cron_job_row_t;

/**
 * Free heap fields on a cron job row. Safe on NULL, zeroed, or already-freed rows.
 *
 * @param row Row to release (may be NULL).
 */
void cron_job_row_free(cron_job_row_t *row);

/**
 * Create a cron job.
 *
 * @return 0 on success, non-zero on error.
 */
int cron_job_create(const char *id, const char *schedule, const char *message,
                   const char *channel, const char *recipient, long long next_run, int enabled);

/**
 * Delete a cron job by id.
 *
 * @return 0 on success, non-zero on error.
 */
int cron_job_delete(const char *id);

/**
 * Toggle enabled flag (0 <-> 1).
 *
 * @return 0 on success, non-zero on error.
 */
int cron_job_toggle(const char *id);

/**
 * Update next_run for a job.
 *
 * @return 0 on success, non-zero on error.
 */
int cron_job_update_next_run(const char *id, long long next_run);

/**
 * List cron jobs into output array.
 *
 * @param out       Array to fill (caller-allocated). Heap fields are owned
 *                  by the caller on success; on -1, no row is owned.
 * @param max_count Maximum jobs to return.
 * @return Number of jobs written, or -1 on error.
 */
int cron_job_list(cron_job_row_t *out, int max_count);

/**
 * Get the next due job (next_run <= now, enabled).
 *
 * @param now Current Unix timestamp.
 * @param out Filled with job data if found. Caller must cron_job_row_free().
 * @return 1 if found, 0 if none, -1 on error.
 */
int cron_job_get_next_due(long long now, cron_job_row_t *out);

/**
 * Release resources and close the database. Safe to call multiple times.
 */
void memory_cleanup(void);

/**
 * SQLite row counts for ASAP @c state.query (sessions, memories, cron_jobs).
 * Any of @p sessions_out, @p memories_out, @p cron_jobs_out may be NULL to skip.
 *
 * @return 0 on success, -1 if the database is not open or a query fails.
 */
int memory_get_row_counts(int *sessions_out, int *memories_out, int *cron_jobs_out);

#ifdef __cplusplus
}
#endif

#endif /* SHELLCLAW_MEMORY_H */
