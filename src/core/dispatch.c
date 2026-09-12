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
		/* Serialize with ASAP agent_run so session_delete cannot race
		 * the same session (see #54, review on #85). Drop the lock
		 * before ch->send so channel I/O does not pin the mutex. */
		agent_lock();
		session_delete(msg->session_id);
		agent_unlock();
		return ch->send(msg->session_id, "Session cleared.", NULL, 0);
	}
	if (strcmp(text, "/status") == 0) {
		char buf[128];
		snprintf(buf, sizeof(buf), "ShellClaw %s — agent ready.",
		         SHELLCLAW_RELEASE_VERSION);
		return ch->send(msg->session_id, buf, NULL, 0);
	}
	char resp_buf[RESPONSE_BUF_SIZE];
	agent_tool_t flat_tools[SHELLCLAW_MAX_TOOLS];
	size_t tool_count = bootstrap_fill_agent_tools(flat_tools, SHELLCLAW_MAX_TOOLS);
	agent_lock();
	int err = agent_run(bootstrap_get_cfg(), msg->session_id, text, bootstrap_get_provider(),
	                    flat_tools, tool_count,
	                    resp_buf, sizeof(resp_buf));
	agent_unlock();
	if (err != 0 && resp_buf[0] == '\0')
		snprintf(resp_buf, sizeof(resp_buf), "Error: agent failed (code %d)", err);
	return ch->send(msg->session_id, resp_buf, NULL, 0);
}
