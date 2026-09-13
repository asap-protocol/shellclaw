/**
 * @file skill.h
 * @brief Skill loader: scan skills directory for .md files and concatenate for system prompt.
 *
 * Skills are loaded once at startup (no hot-reload in Phase 1). Used by the agent
 * to build the system prompt after SOUL.md and IDENTITY.md.
 */

#ifndef SHELLCLAW_SKILL_H
#define SHELLCLAW_SKILL_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

struct config;
typedef struct config config_t;

/**
 * Load all .md skill files from the configured skills directory and concatenate
 * their contents into out_buf, separated by "\n\n---\n\n".
 *
 * If the skills directory does not exist or is empty, writes an empty string
 * and returns 0 (success). Caller should treat empty output as "no skills".
 * If out_buf is too small, output is truncated and null-terminated; still returns 0.
 *
 * @param cfg       Configuration (must not be NULL; uses config_skills_dir(cfg)).
 * @param out_buf   Buffer to receive concatenated skill content (null-terminated).
 * @param out_size  Size of out_buf (must be > 0).
 * @return 0 on success, -1 on error (e.g. cfg NULL or out_buf NULL or out_size 0).
 */
int skill_load_all(const config_t *cfg, char *out_buf, size_t out_size);

/**
 * Build the system prompt base: SOUL content + IDENTITY content + skills content,
 * in that order. Used by the agent to assemble the full system prompt.
 * Missing SOUL/IDENTITY files are skipped with a warning (graceful degradation).
 *
 * @param cfg             Configuration (soul_path, identity_path from config).
 * @param skills_content  Pre-loaded skills string (from skill_load_all); may be NULL or empty.
 * @param out_buf         Buffer to receive the concatenated content (null-terminated).
 * @param out_size        Size of out_buf (must be > 0).
 * @return 0 on success, -1 on error (e.g. cfg NULL, out_buf NULL, out_size 0).
 */
int skill_build_system_prompt_base(const config_t *cfg, const char *skills_content,
                                  char *out_buf, size_t out_size);

/**
 * List skill names (base names of .md files without extension).
 *
 * @param cfg             Configuration (uses config_skills_dir).
 * @param names_out       Array of pointers to receive names; caller must free each.
 * @param max_count       Maximum number of names to return.
 * @return Number of skills returned, or -1 on error.
 */
int skill_list_names(const config_t *cfg, char **names_out, int max_count);

/**
 * Get content of a skill by name (file base name without .md).
 *
 * @param cfg             Configuration (uses config_skills_dir).
 * @param name            Skill name (file base name).
 * @param out_buf         Buffer to receive content.
 * @param out_size        Size of out_buf.
 * @return 0 on success, -1 if not found or error.
 */
int skill_get_content(const config_t *cfg, const char *name, char *out_buf, size_t out_size);

/**
 * Short description for ASAP manifest skills: config override, else first line of the .md file
 * (leading "# " stripped), else NULL when none available.
 *
 * @param cfg       Configuration.
 * @param name      Skill id (file base name without .md).
 * @param out_buf   Buffer for description text.
 * @param out_size  Size of out_buf.
 * @return 0 when a description was written, -1 on error or missing description.
 */
int skill_get_description(const config_t *cfg, const char *name, char *out_buf, size_t out_size);

/**
 * Create a new skill file.
 *
 * @param cfg             Configuration (uses config_skills_dir).
 * @param name            Skill name (file base name; .md appended).
 * @param content         Content to write.
 * @return 0 on success, non-zero on error.
 */
int skill_create(const config_t *cfg, const char *name, const char *content);

/**
 * Update an existing skill file.
 *
 * @param cfg             Configuration (uses config_skills_dir).
 * @param name            Skill name (file base name).
 * @param content         Content to write.
 * @return 0 on success, non-zero on error.
 */
int skill_update(const config_t *cfg, const char *name, const char *content);

/**
 * Delete a skill file.
 *
 * @param cfg             Configuration (uses config_skills_dir).
 * @param name            Skill name (file base name).
 * @return 0 on success, non-zero on error.
 */
int skill_delete(const config_t *cfg, const char *name);

/**
 * Start watching the skills directory for changes (hot-reload).
 * Uses inotify on Linux, kqueue on macOS; no-op if dir missing.
 * When enabled, skill_load_all returns cached content updated on file changes.
 *
 * @param cfg    Configuration (skills_dir, soul, identity paths).
 * @param verbose If non-zero, log changes to stderr.
 * @return 0 on success, -1 if watch could not be started (e.g. dir missing).
 */
int skill_watch_start(const config_t *cfg, int verbose);

/**
 * Stop the skills watcher and free cached data.
 */
void skill_watch_stop(void);

#ifdef __cplusplus
}
#endif

#endif /* SHELLCLAW_SKILL_H */
