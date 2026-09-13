/**
 * @file registry.c
 * @brief Tool registry: tool_set_config, tool_get_all.
 */

#include "tools/tool.h"
#include "tools/shell.h"
#include "tools/web_search.h"
#include "tools/file.h"
#include "tools/cron.h"
#include "tools/asap_invoke.h"
#include "tools/context.h"
#include "tools/hardware_tools.h"
#include "hardware/hardware.h"
#include "core/config.h"
#include <stddef.h>

void tool_set_config(const config_t *cfg)
{
	tool_shell_set_config(cfg);
	tool_file_set_config(cfg);
	tool_web_search_set_config(cfg);
	tool_asap_invoke_set_config(cfg);
	tool_context_set_config(cfg);
	hardware_init(cfg);
	tool_hardware_set_config(cfg);
}

size_t tool_get_all(const tool_t **out, size_t max_count)
{
	if (!out || max_count == 0) return 0;
	size_t n = 0;
	const tool_t *shell = tool_shell_get();
	const tool_t *web = tool_web_search_get();
	const tool_t *file = tool_file_get();
	const tool_t *cron = tool_cron_get();
	const tool_t *asap_invoke = tool_asap_invoke_get();
	const tool_t *ctx = tool_context_get();
	/* cppcheck-suppress knownConditionTrueFalse */
	if (n < max_count && shell) out[n++] = shell;
	if (n < max_count && web) out[n++] = web;
	if (n < max_count && file) out[n++] = file;
	if (n < max_count && cron)
		out[n++] = cron;
	if (n < max_count && ctx)
		out[n++] = ctx;
	if (n < max_count && asap_invoke)
		out[n++] = asap_invoke;
	n += tool_hardware_get_all(out + n, max_count - n);
	return n;
}
