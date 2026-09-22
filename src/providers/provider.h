/**
 * @file provider.h
 * @brief Provider vtable and shared types. Keys from env via config; never logged.
 */

#ifndef SHELLCLAW_PROVIDER_H
#define SHELLCLAW_PROVIDER_H

#include <stddef.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

struct config;
typedef struct config config_t;

/** Tool definition passed to the LLM (JSON Schema for parameters). */
typedef struct provider_tool_def {
	const char *name;             /**< Tool name (e.g. "shell") */
	const char *description;      /**< Human-readable description */
	const char *parameters_json;  /**< JSON Schema string for arguments */
} provider_tool_def_t;

/** One tool call returned by the LLM (provider allocates; caller frees via response_clear). */
typedef struct provider_tool_call {
	char *id;         /**< Call id from API */
	char *name;       /**< Tool name to invoke */
	char *arguments;  /**< JSON string of arguments */
} provider_tool_call_t;

/** Single chat message (caller-owned; provider does not take ownership). */
typedef struct provider_message {
	const char *role;   /**< "system", "user", "assistant", or "tool" (OpenAI) */
	const char *content; /**< Message body (text) */
	/** For assistant messages: tool calls made in this turn (provider builds tool_use blocks). */
	const provider_tool_call_t *tool_calls;
	size_t tool_calls_count;
	/** For tool result messages: id of the tool_use this result answers (content = result). */
	const char *tool_use_id;
} provider_message_t;

/** Chat response (provider fills; caller must call provider_response_clear after use). */
typedef struct provider_response {
	int error;                    /**< 0 on success, non-zero on failure */
	char *content;                /**< Assistant text (allocated by provider) */
	provider_tool_call_t *tool_calls; /**< Array of tool calls (allocated by provider) */
	size_t tool_calls_count;      /**< Number of tool_calls */
} provider_response_t;

/** Clear and free response fields. Caller-allocated struct; safe to call repeatedly or on zeroed. */
void provider_response_clear(provider_response_t *r);

/* --- Shared curl helpers for providers --- */

#define PROVIDER_RESP_BUF_INIT 65536

/** Growable buffer for curl write callbacks. */
typedef struct provider_curl_buf {
	char *buf;
	size_t len;
	size_t cap;
} provider_curl_buf_t;

/** Curl write callback using provider_curl_buf_t. Cap: PROVIDER_RESP_BUF_INIT * 4. */
size_t provider_write_cb(const char *ptr, size_t size, size_t nmemb, void *userdata);

/** Set error flag and optional message on response. */
void provider_set_error(provider_response_t *response, const char *msg);

/** strdup-equivalent for provider use. Returns NULL on NULL input. */
char *provider_dup_str(const char *s);

/**
 * Parse OpenAI-style chat/completions JSON into @p response. Caller must provider_response_clear on success.
 * Returns -1 on invalid JSON or API error object.
 */
int provider_parse_chat_completions_json(const char *response_body, provider_response_t *response);

/** Non-zero if router may try next chain entry: no `HTTP 4xx` in message, otherwise transport/5xx may retry. */
int provider_error_allows_fallback_retry(const char *provider_error_content);

void provider_router_active_backend_snapshot(char *dst, size_t dst_sz);
void provider_router_last_error_snapshot(char *dst, size_t dst_sz);
void provider_router_periodic_recovery_tick(time_t now_wall);
void provider_router_periodic_recovery_set_interval_seconds(unsigned interval_sec);
void provider_router_periodic_recovery_reset_timer(void);
/** Malloc'd JSON for GET /api/status; caller frees. NULL on OOM. */
char *provider_router_api_status_json(void);

typedef void (*provider_router_status_changed_fn)(void);
void provider_router_set_status_changed_callback(provider_router_status_changed_fn fn);

/**
 * After reloading config.toml (same process): point the fallback router's live reads at @p cfg.
 * Does not re-run backend init; chain shape and backends stay as at startup (PRD §9 Q4).
 */
void provider_router_set_live_config(const config_t *cfg);

typedef struct provider {
	const char *name;
	/** Initialize with config (e.g. read API key env). Return 0 on success. */
	int (*init)(const config_t *cfg);
	/**
	 * Send messages and optional tools; fill response. Return 0 on success.
	 * On failure, set response->error and optionally response->content to error message.
	 */
	int (*chat)(const provider_message_t *messages, size_t message_count,
	            const provider_tool_def_t *tools, size_t tool_count,
	            provider_response_t *response);
	/** Release provider resources. */
	void (*cleanup)(void);
} provider_t;

/** Stub provider for tests and vtable verification. Returns static vtable. */
const provider_t *provider_stub_get(void);
/** Second stub backend with optional forced chat failure (router / recovery tests). */
const provider_t *provider_stub_b_get(void);
void provider_stub_b_set_chat_should_fail(int should_fail);
/** Override stub-b chat error text when ``should_fail`` is set (test literals only). */
void provider_stub_b_set_chat_error_message(const char *message);


/** Anthropic (Claude) provider. Messages API, tool_use. */
const provider_t *provider_anthropic_get(void);

/** OpenAI provider. Chat Completions, tool_calls (function calling). */
const provider_t *provider_openai_get(void);
/** After SIGHUP reload: refresh live config reads without re-init (PRD §9 Q4). */
void provider_openai_set_live_config(const config_t *cfg);

/** Local llama.cpp / OpenAI-compatible HTTP server (#config_provider_local_endpoint). */
const provider_t *provider_local_get(void);
/** After SIGHUP reload: refresh live config reads without re-init (PRD §9 Q4). */
void provider_local_set_live_config(const config_t *cfg);
/** Lightweight GET probe via `/v1/models` or `/health`; used by init and router recovery. */
int provider_local_endpoint_reachable(const char *endpoint);
/** After a successful recovery GET probe on local; clears init-time unreachable flag. */
void provider_local_recovery_probe_succeeded(void);

#ifdef SHELLCLAW_TEST
int local_provider_is_unavailable_for_test(void);
void local_provider_test_reset(void);
void local_provider_test_set_http_response(const char *json_body);
void local_provider_test_set_skip_probe(int skip);
#endif

const provider_t *provider_router_get(const config_t *cfg);

#ifdef __cplusplus
}
#endif

#endif /* SHELLCLAW_PROVIDER_H */
