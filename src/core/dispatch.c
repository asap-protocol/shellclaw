/**
 * @file dispatch.c
 * @brief Slash commands and agent message dispatch.
 */
#define _POSIX_C_SOURCE 200809L

#include "core/dispatch.h"
#include "core/agent.h"
#include "core/bootstrap.h"
#include "core/memory.h"
#include "core/version.h"
#include <stdio.h>
#include <string.h>

#define RESPONSE_BUF_SIZE (32 * 1024)

int handle_message(const channel_t *ch, const channel_incoming_msg_t *msg)
{
	const char *text = msg->text ? msg->text : "";
	if (strcmp(text, "/reset") == 0) {
		session_delete(msg->session_id);
		return ch->send(msg->session_id, "Session cleared.", NULL, 0);
	}
	if (strcmp(text, "/status") == 0) {
		char buf[128];
		snprintf(buf, sizeof(buf), "ShellClaw %s — agent ready.",
		         SHELLCLAW_RELEASE_VERSION);
		return ch->send(msg->session_id, buf, NULL, 0);
	}
	char resp_buf[RESPONSE_BUF_SIZE];
	size_t tool_count = bootstrap_tool_count();
	agent_tool_t flat_tools[SHELLCLAW_MAX_TOOLS];
	if (tool_count > SHELLCLAW_MAX_TOOLS)
		tool_count = SHELLCLAW_MAX_TOOLS;
	for (size_t i = 0; i < tool_count; i++) {
		const tool_t *t = bootstrap_tool_at(i);
		if (!t)
			break;
		flat_tools[i].name = t->name;
		flat_tools[i].description = t->description;
		flat_tools[i].parameters_json = t->parameters_json;
		flat_tools[i].execute = t->execute;
	}
	int err = agent_run(bootstrap_get_cfg(), msg->session_id, text, bootstrap_get_provider(),
	                    flat_tools, tool_count,
	                    resp_buf, sizeof(resp_buf));
	if (err != 0 && resp_buf[0] == '\0')
		snprintf(resp_buf, sizeof(resp_buf), "Error: agent failed (code %d)", err);
	return ch->send(msg->session_id, resp_buf, NULL, 0);
}
