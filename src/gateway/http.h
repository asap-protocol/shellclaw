/**
 * @file http.h
 * @brief HTTP server and REST API (libwebsockets for HTTP+WebSocket same port).
 */

#ifndef SHELLCLAW_GATEWAY_HTTP_H
#define SHELLCLAW_GATEWAY_HTTP_H

#ifdef __cplusplus
extern "C" {
#endif

struct config;
typedef struct config config_t;
struct auth_ctx;

/**
 * Start HTTP+WebSocket server on config host:port.
 * Binds the listen socket to config_gateway_host (default 127.0.0.1 via
 * info.iface). Rejects host 0.0.0.0/"*" unless allow_bind_all is true.
 *
 * @param cfg         Configuration (host, port, allow_bind_all).
 * @param auth_ctx    Auth context for token validation.
 * @param config_path Path to config.toml for PUT /api/config.
 * @return 0 on success, non-zero on error.
 */
int http_start(const config_t *cfg, struct auth_ctx *auth_ctx, const char *config_path);

/**
 * Stop HTTP server. Safe to call if not started.
 */
void http_stop(void);

/** Push provider_status WebSocket JSON (same payload as GET /api/status plus `type`). */
void http_emit_ws_provider_status(void);

/**
 * After SIGHUP config reload: swap the server's live config pointer (no HTTP rebind).
 * No-op when the gateway was not started.
 */
void http_set_live_config(const config_t *cfg);

#ifdef __cplusplus
}
#endif

#endif /* SHELLCLAW_GATEWAY_HTTP_H */
