/**
 * @file server.h
 * @brief Inbound ASAP dispatch: payload routing, optional hooks for tests.
 */
#ifndef SHELLCLAW_ASAP_SERVER_H
#define SHELLCLAW_ASAP_SERVER_H

#include "asap/envelope.h"
#include "cJSON.h"
#include "core/agent.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

struct config;
typedef struct config config_t;

struct provider;
typedef struct provider provider_t;

/**
 * Context for inbound ASAP handling. Production sets @p cfg and @p provider;
 * tests may leave them NULL and use hooks instead.
 */
typedef struct asap_server_ctx {
	const config_t *cfg;
	const provider_t *provider;
	const agent_tool_t *tools;
	size_t tool_count;
	const char *session_id;
	int (*task_request_hook)(const struct asap_server_ctx *ctx, const asap_envelope_t *in,
				char *response_buf, size_t response_cap);
	int (*tool_call_hook)(const struct asap_server_ctx *ctx, const char *tool_name,
			      const char *args_json, char *result_buf, size_t result_cap);
	int (*state_query_hook)(const struct asap_server_ctx *ctx, cJSON **payload_out);
} asap_server_ctx_t;

/** Application-defined JSON-RPC error when inbound sender URN is not on the trust list. */
#define ASAP_SERVER_RPC_SENDER_UNTRUSTED (-32002)

/** Application-defined JSON-RPC error when an MCP tool name is unknown. */
#define ASAP_SERVER_RPC_TOOL_NOT_FOUND (-32001)

/**
 * Handle one inbound ASAP envelope and fill @p out (caller clears after use).
 *
 * @param in               Parsed request envelope (non-NULL).
 * @param out              Initialized output envelope on success.
 * @param ctx              Server context (non-NULL).
 * @param err_message      Optional buffer for a short diagnostic when returning &lt; 0.
 * @param err_message_size Size of @p err_message.
 * @return                 0 on success; negative JSON-RPC error code on failure.
 */
int asap_server_handle(const asap_envelope_t *in, asap_envelope_t *out,
			asap_server_ctx_t *ctx, char *err_message, size_t err_message_size);

/**
 * Resolve the agent session id for inbound task.request.
 * Derives `asap:<sender>` from the envelope sender URN so unrelated clients
 * do not share SQLite history (#64). Missing sender falls back to
 * `asap:inbound`. Returns NULL when @p buf cannot hold the derived id
 * (fail closed; do not truncate).
 *
 * Example: asap_resolve_task_session_id("urn:alice", buf, sizeof buf)
 * returns "asap:urn:alice".
 */
const char *asap_resolve_task_session_id(const char *sender, char *buf, size_t buf_size);

#ifdef __cplusplus
}
#endif

#endif /* SHELLCLAW_ASAP_SERVER_H */
