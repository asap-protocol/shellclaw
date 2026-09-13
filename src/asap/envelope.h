/**
 * @file envelope.h
 * @brief ASAP v2.1 envelope types and JSON-RPC 2.0 error helpers.
 */
#ifndef SHELLCLAW_ASAP_ENVELOPE_H
#define SHELLCLAW_ASAP_ENVELOPE_H

#include "asap/asap_version.h"
#include "cJSON.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Single ASAP message as structured fields. String fields, @p payload, and
 * @p jsonrpc_request_id are owned when filled by API functions; use
 * asap_envelope_clear to release.
 */
typedef struct asap_envelope {
	cJSON *jsonrpc_request_id; /**< Top-level JSON-RPC "id" (any JSON type); for responses/errors. */
	char *id;             /**< Envelope or task id (ULID or string). */
	char *asap_version;  /**< Protocol version, e.g. "2.1". */
	char *sender;        /**< Sender URN. */
	char *recipient;     /**< Recipient URN. */
	char *payload_type;  /**< e.g. task.request, task.response. */
	cJSON *payload;      /**< JSON object or value; owned; may be NULL. */
	char *correlation_id; /**< Correlation for tracing. */
	char *trace_id;      /**< Distributed trace id. */
	char *timestamp;     /**< ISO 8601 timestamp string. */
} asap_envelope_t;

/**
 * Zero-initialize an envelope. Does not free previous contents;
 * use asap_envelope_clear first if the struct was already in use.
 *
 * @param env  Envelope to initialize (non-NULL).
 */
void asap_envelope_init(asap_envelope_t *env);

/**
 * Free all string fields and the payload cJSON. Safe on cleared or
 * init-only structures.
 *
 * @param env  Envelope to clear.
 */
void asap_envelope_clear(asap_envelope_t *env);

/**
 * Parse a JSON-RPC 2.0 request body. Top-level @c jsonrpc must be @c "2.0",
 * @c method a non-empty string, @c params an object with required ASAP
 * fields: @c id, @c asap_version, @c sender, @c recipient, @c payload_type,
 * and @c payload. Optional: @c correlation_id, @c trace_id, @c timestamp
 * (strings). On success, @p out is filled; call asap_envelope_clear when
 * done. On failure, out may be cleared and, if @p err_out is non-NULL,
 * *err_out is a full JSON-RPC error (code -32602 Invalid params) with
 * the request @c id echoed; caller must cJSON_Delete.
 *
 * @param json     NUL-terminated request body
 * @param out      Envelope to fill (any prior contents should be cleared first)
 * @param err_out  Optional: receives error object on failure
 * @return         0 on success, -1 on validation or parse error
 */
int asap_envelope_parse(const char *json, asap_envelope_t *out, cJSON **err_out);

/**
 * Load envelope fields from a plain JSON object (same key layout as
 * JSON-RPC @c params / @c result). Used for result bodies and tests.
 * On failure clears @p out and may set *err_out to a -32602 JSON-RPC
 * error. On success, @p out owns strings and payload; does not set
 * jsonrpc_request_id.
 *
 * @param obj     Object with ASAP envelope members (not NULL)
 * @param rpc_id  Request id for error echo. On validation failure this
 *                pointer is freed. On success it is not consumed; the
 *                caller may assign it to out->jsonrpc_request_id.
 * @param out     Envelope; must be zeroed (#asap_envelope_init) or cleared first
 *                because the implementation clears the struct on entry.
 * @param err_out Optional error root
 * @return        0 on success, -1 on error
 */
int asap_envelope_from_object(const cJSON *obj, cJSON *rpc_id, asap_envelope_t *out, cJSON **err_out);

/**
 * Build a JSON-RPC 2.0 success response: @c { "jsonrpc": "2.0", "result": { ...envelope... }, "id": ... }.
 * The @a result object mirrors the request @c params shape. If @a jsonrpc_id
 * is NULL, uses @c env->jsonrpc_request_id. Caller cJSON_Deletes the return
 * or uses #asap_envelope_to_jsonrpc_string.
 *
 * @return Root object, or NULL if required fields are missing/invalid
 */
cJSON *asap_envelope_to_jsonrpc(const asap_envelope_t *env, cJSON *jsonrpc_id);

/**
 * Same as #asap_envelope_to_jsonrpc, then cJSON_PrintUnformatted. Caller must free(3) the string.
 * @return Allocated string, or NULL
 */
char *asap_envelope_to_jsonrpc_string(const asap_envelope_t *env, cJSON *jsonrpc_id);

/**
 * Build JSON-RPC 2.0 @e request: @c { "jsonrpc": "2.0", "method", "params", "id" }.
 * @param method  e.g. @c "asap.send"; if NULL or empty, uses @c "asap.send"
 */
cJSON *asap_envelope_to_jsonrpc_request(const asap_envelope_t *env, cJSON *jsonrpc_id, const char *method);

char *asap_envelope_to_jsonrpc_request_string(const asap_envelope_t *env, cJSON *jsonrpc_id, const char *method);

/**
 * Parse a JSON-RPC 2.0 @e success response (HTTP body): @c "result" object with envelope fields;
 * @c "error" present yields failure. Sets @a out as #asap_envelope_from_object on @c result and
 * copies @c "id" to @a out->jsonrpc_request_id.
 * @return 0 on success; -1 and optional @a errmsg (truncated) on failure
 */
int asap_envelope_parse_jsonrpc_response(const char *json, asap_envelope_t *out, char *errmsg, size_t errlen);

/**
 * Build a JSON-RPC 2.0 error object (top-level) with a standard error
 * body. The returned root must be freed with cJSON_Delete, or use
 * cJSON_Print and free the string.
 *
 * @param code     JSON-RPC error code (e.g. -32602).
 * @param message  Error message (non-NULL).
 * @param id       Request id to echo; may be NULL for null id.
 * @return         Root cJSON object, or NULL on allocation failure.
 */
cJSON *asap_jsonrpc_error(int code, const char *message, cJSON *id);

#ifdef __cplusplus
}
#endif

#endif /* SHELLCLAW_ASAP_ENVELOPE_H */
