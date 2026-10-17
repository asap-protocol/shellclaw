/**
 * @file dispatch.h
 * @brief Incoming message dispatch: slash commands and agent run.
 */

#ifndef SHELLCLAW_DISPATCH_H
#define SHELLCLAW_DISPATCH_H

#include "channels/channel.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Handle an incoming channel message (/reset, /status, or agent run).
 * @return Result of channel send(), or -1 on internal error.
 */
int handle_message(const channel_t *ch, const channel_incoming_msg_t *msg);

/**
 * Delete one session while holding the agent mutex.
 *
 * Dashboard `DELETE /api/sessions/:id` runs on the gateway thread. agent_run()
 * loads history, talks to the model, then session_save()s. Without this lock
 * the delete lands in that window and the save puts the old transcript back.
 *
 * Example: `if (dispatch_delete_session(id) != 0) respond 404;`
 *
 * @param session_id Session row id. Empty or NULL is rejected.
 * @return 0 when a row was removed, non-zero otherwise.
 */
int dispatch_delete_session(const char *session_id);

#ifdef __cplusplus
}
#endif

#endif /* SHELLCLAW_DISPATCH_H */
