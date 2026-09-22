/**
 * @file tool_reload_stub.c
 * @brief No-op tool_set_config for reload unit tests (reload.c calls channel/provider hooks directly).
 */
#include "tools/tool.h"

void tool_set_config(const config_t *cfg)
{
	(void)cfg;
}
