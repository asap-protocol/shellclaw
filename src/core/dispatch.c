/**
 * @file dispatch.c
 * @brief Slash commands and agent message dispatch.
 */
#define _POSIX_C_SOURCE 200809L

#include "channels/heartbeat.h"
#include "channels/webchat.h"
#include "core/dispatch.h"
#include "core/agent.h"
#include "core/bootstrap.h"
#include "core/memory.h"
#include "core/version.h"
#include "providers/provider.h"
#include "tools/cron.h"
#include <stdio.h>
#include <string.h>

#define RESPONSE_BUF_SIZE (32 * 1024)

static int cron_target_available(const char *session_id)
{
	const char *separator = session_id ? strchr(session_id, ':') : NULL;
	char name[64];
	if (!separator)
		return 0;
	size_t length = (size_t)(separator - session_id);
	if (length >= sizeof(name))
		return 0;
	memcpy(name, session_id, length);
	name[length] = '\0';
	const channel_t *target = channel_get_by_name(name);
	if (strcmp(name, "heartbeat") == 0 || target == channel_heartbeat_get())
		target = target == channel_heartbeat_get() ? heartbeat_local_delivery() : NULL;
	return target && target->send;
}

static int maybe_ack_cron(const char *job_id, const channel_incoming_msg_t *silent_reply)
{
	int ack_err;
	if (!job_id)
		return 0;
	/* Check the current destination and acknowledge a silent reply under one lock. */
	agent_lock();
	if (silent_reply && !cron_target_available(silent_reply->session_id))
		ack_err = -1;
	else
		ack_err = cron_ack_delivery(job_id);
	agent_unlock();
	return ack_err;
}

static int send_then_maybe_ack_cron(const channel_t *ch, const channel_incoming_msg_t *msg,
				    const char *text, int agent_err)
{
	const char *cron_job_id;
	int send_err;
	if (!ch || (!ch->reply && !ch->send) || !msg)
		return -1;
	cron_job_id = ch->name && strcmp(ch->name, "cron") == 0 ? msg->user_id : NULL;
	/* Keep a silent job due until its destination is available. */
	if (cron_job_id && agent_err == 0 && provider_text_is_blank(text))
		return maybe_ack_cron(cron_job_id, msg);
	send_err = ch->reply ? ch->reply(msg, text) : ch->send(msg->session_id, text, NULL, 0);
	if (send_err != 0)
		return send_err;
	return agent_err == 0 ? maybe_ack_cron(cron_job_id, NULL) : 0;
}

int dispatch_delete_session(const char *session_id)
{
	int rc;
	if (!session_id || session_id[0] == '\0')
		return -1;
	/* Serialize with agent_run so session_save cannot recreate the row. */
	agent_lock();
	rc = session_delete(session_id);
	agent_unlock();
	return rc;
}

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
		const char *reply = "Session cleared.";
		if (msg->session_id && strncmp(msg->session_id, WEBCHAT_DEVICE_PREFIX, sizeof(WEBCHAT_DEVICE_PREFIX) - 1) == 0)
			reply = "Model context cleared. The transcript stays visible.";
		return send_then_maybe_ack_cron(ch, msg, reply, 0);
	}
	if (strcmp(text, "/status") == 0) {
		char buf[128];
		snprintf(buf, sizeof(buf), "ShellClaw %s — agent ready.",
		         SHELLCLAW_RELEASE_VERSION);
		return send_then_maybe_ack_cron(ch, msg, buf, 0);
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
	return send_then_maybe_ack_cron(ch, msg, resp_buf, err);
}
