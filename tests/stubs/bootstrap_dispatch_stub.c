/**
 * @file bootstrap_dispatch_stub.c
 * @brief Minimal bootstrap surface for dispatch/reload unit tests (avoids full subsystem init).
 */
#define _POSIX_C_SOURCE 200809L

#include "core/agent.h"
#include "core/bootstrap.h"
#include "providers/provider.h"
#include "tools/tool.h"
#include <stdio.h>
#include <string.h>

static config_t *g_cfg;
static const provider_t *g_provider;
static const tool_t *g_tools[SHELLCLAW_MAX_TOOLS];
static size_t g_tool_count;
static char g_config_path[512];

config_t *bootstrap_get_cfg(void)
{
	return g_cfg;
}

void bootstrap_set_cfg(config_t *cfg)
{
	g_cfg = cfg;
}

void bootstrap_set_config_path(const char *path)
{
	if (path != NULL)
		snprintf(g_config_path, sizeof(g_config_path), "%s", path);
	else
		g_config_path[0] = '\0';
}

const char *bootstrap_get_config_path(void)
{
	return (g_config_path[0] != '\0') ? g_config_path : NULL;
}

const provider_t *bootstrap_get_provider(void)
{
	return g_provider;
}

void bootstrap_set_provider_for_test(const provider_t *provider)
{
	g_provider = provider;
}

size_t bootstrap_tool_count(void)
{
	return g_tool_count;
}

const tool_t *bootstrap_tool_at(size_t index)
{
	if (index >= g_tool_count)
		return NULL;
	return g_tools[index];
}

size_t bootstrap_fill_agent_tools(agent_tool_t *out, size_t cap)
{
	size_t tool_count = g_tool_count;
	size_t i;
	if (!out || cap == 0)
		return 0;
	if (tool_count > cap)
		tool_count = cap;
	for (i = 0; i < tool_count; i++) {
		const tool_t *t = g_tools[i];
		if (!t) {
			tool_count = i;
			break;
		}
		out[i].name = t->name;
		out[i].description = t->description;
		out[i].parameters_json = t->parameters_json;
		out[i].execute = t->execute;
	}
	return tool_count;
}

void bootstrap_reset_tools_for_test(void)
{
	g_tool_count = 0;
}

void bootstrap_add_tool_for_test(const tool_t *tool)
{
	if (tool != NULL && g_tool_count < SHELLCLAW_MAX_TOOLS)
		g_tools[g_tool_count++] = tool;
}
