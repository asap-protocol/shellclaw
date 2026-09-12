/**
 * @file agent.h
 * @brief ReAct agent loop: load context, call LLM, execute tools, return response. Single entry point; provider and tool vtables.
 */

#ifndef SHELLCLAW_AGENT_H
#define SHELLCLAW_AGENT_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

struct config;
typedef struct config config_t;

struct provider;
typedef struct provider provider_t;

/** Tool vtable (shell, web_search, file — Task 7). Agent calls execute() by name. */
typedef struct agent_tool {
	const char *name;
	const char *description;
	const char *parameters_json;
	/**
	 * Execute tool with JSON arguments. Write result into result_buf (max_len bytes).
	 * @return 0 on success, non-zero on error.
	 */
	int (*execute)(const char *args_json, char *result_buf, size_t max_len);
} agent_tool_t;

/**
 * Run the agent loop: load context, call LLM, execute tool calls until done or max iterations.
 * Response text is written to response_buf (null-terminated); caller owns the buffer.
 *
 * @param cfg           Configuration (model, limits, paths).
 * @param session_id    Session ID (e.g. "cli:default", "telegram:123"); used for history.
 * @param user_message  User message text (must not be NULL).
 * @param provider      Fallback composite (`shellclaw-router`), e.g. from provider_router_get(cfg); must not be NULL.
 * @param tools         Array of tools (may be NULL if tool_count is 0).
 * @param tool_count    Number of tools.
 * @param response_buf  Output buffer for final assistant text (or error message).
 * @param response_size Size of response_buf (must be > 0).
 * @return 0 on success, non-zero on error (response_buf may contain error message).
 */
int agent_run(const config_t *cfg, const char *session_id, const char *user_message,
              const provider_t *provider, const agent_tool_t *tools, size_t tool_count,
              char *response_buf, size_t response_size);

/**
 * Acquire the global agent mutex before touching shared session/memory state.
 *
 * Every agent_run() caller (main-loop handle_message, inbound ASAP HTTP,
 * WebSocket dispatcher) must hold this mutex for the duration of agent_run().
 * /reset in handle_message must also hold it around session_delete().
 * Inbound mcp.tool_call must hold it around tool execute (see #60).
 * Release before channel I/O (ch->send). The mutex is not recursive.
 */
void agent_lock(void);

/**
 * Release the global agent mutex after agent_run() or a locked session_delete().
 */
void agent_unlock(void);

/** Test-only: non-empty @p name_or_null forces “active backend” for local/offline prompt suffix; NULL uses router. */
void shellclaw_agent_set_test_active_backend_name(const char *name_or_null);

/**
 * Test-only: probe whether the global agent mutex is currently held.
 * @return 1 if locked, 0 if free.
 *
 * Relies on a non-recursive mutex: pthread_mutex_trylock from the owning
 * thread returns EBUSY. A recursive mutex would make this helper lie
 * (trylock succeeds, this function unlocks once, returns 0).
 *
 * Used by dispatch tests to assert handle_message() holds the mutex
 * for the duration of agent_run() (see #54), and by ASAP server tests
 * for inbound mcp.tool_call (#60).
 */
int agent_mutex_is_locked_for_test(void);

#ifdef __cplusplus
}
#endif

#endif /* SHELLCLAW_AGENT_H */
