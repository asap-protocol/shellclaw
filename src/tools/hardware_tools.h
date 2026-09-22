/**
 * @file hardware_tools.h
 * @brief GPIO, I2C, and camera tools for the agent registry.
 */

#ifndef SHELLCLAW_TOOLS_HARDWARE_H
#define SHELLCLAW_TOOLS_HARDWARE_H

#include "tools/tool.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

struct config;
typedef struct config config_t;

/** Bind config for hardware tools (called from tool registry after hardware_init). */
void tool_hardware_set_config(const config_t *cfg);

/**
 * Copy hardware tool pointers into @p out when [hardware] enabled.
 *
 * @return Number of tools written (0 when disabled or @p out is NULL).
 */
size_t tool_hardware_get_all(const tool_t **out, size_t max_count);

#ifdef __cplusplus
}
#endif

#endif /* SHELLCLAW_TOOLS_HARDWARE_H */
