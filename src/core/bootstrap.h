/**
 * @file bootstrap.h
 * @brief Subsystem init/cleanup: memory, skills, providers, channels, tools, gateway.
 */

#ifndef SHELLCLAW_BOOTSTRAP_H
#define SHELLCLAW_BOOTSTRAP_H

#include "channels/channel.h"
#include "core/agent.h"
#include "core/config.h"
#include "providers/provider.h"
#include "tools/tool.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void bootstrap_set_verbose(int verbose);
void bootstrap_set_cli_one_shot(const char *one_shot);
void bootstrap_set_config_path(const char *path);
const char *bootstrap_get_config_path(void);

/** Initialize memory, skills, providers, channels, gateway, and tools. */
int init_subsystems(config_t *cfg);

/** Register tools from config (called from init_subsystems). */
int tools_init(const config_t *cfg);

/** Tear down all subsystems in reverse init order (HTTP thread before auth_ctx). */
void cleanup_subsystems(void);

config_t *bootstrap_get_cfg(void);
void bootstrap_set_cfg(config_t *cfg);
const provider_t *bootstrap_get_provider(void);

int bootstrap_channel_count(void);
const channel_t *bootstrap_channel_at(int index);

size_t bootstrap_tool_count(void);
const tool_t *bootstrap_tool_at(size_t index);

/**
 * Copy registered tools into an agent_tool_t table for agent_run / ASAP.
 * Caps at @p cap and stops on a NULL slot so the returned count never
 * overruns the filled prefix.
 *
 * Example: n = bootstrap_fill_agent_tools(flat, SHELLCLAW_MAX_TOOLS);
 */
size_t bootstrap_fill_agent_tools(agent_tool_t *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* SHELLCLAW_BOOTSTRAP_H */
