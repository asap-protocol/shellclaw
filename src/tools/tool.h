/**
 * @file tool.h
 * @brief Tool vtable and built-in tools registry (shell, web_search, file).
 */

#ifndef SHELLCLAW_TOOL_H
#define SHELLCLAW_TOOL_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

struct config;
typedef struct config config_t;

/** Tool vtable compatible with agent_tool_t. */
typedef struct tool {
	const char *name;
	const char *description;
	const char *parameters_json;
	int (*execute)(const char *args_json, char *result_buf, size_t max_len);
} tool_t;

/** Agent tool table capacity: 6 core tools + 7 hardware tools, with headroom. */
#define SHELLCLAW_MAX_TOOLS 16

/** Set config for tools that need it (timeout, workspace). Call before tool_get_all. */
void tool_set_config(const config_t *cfg);

/**
 * Get all built-in tools.
 *
 * @param out       Array to receive tool pointers (caller-allocated, at least max_count elements).
 * @param max_count Maximum number of tools to return.
 * @return Number of tools written to out.
 */
size_t tool_get_all(const tool_t **out, size_t max_count);

#ifdef __cplusplus
}
#endif

#endif /* SHELLCLAW_TOOL_H */
