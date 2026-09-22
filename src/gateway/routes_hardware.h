/**
 * @file routes_hardware.h
 * @brief REST handlers for /api/hardware/... (Phase 5 Web UI).
 */

#ifndef SHELLCLAW_GATEWAY_ROUTES_HARDWARE_H
#define SHELLCLAW_GATEWAY_ROUTES_HARDWARE_H

#include <stddef.h>

struct lws;
typedef struct http_server_ctx http_server_ctx_t;

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Dispatch /api/hardware/... routes.
 * @return 1 if @p uri was handled, 0 if not a hardware API path.
 */
int routes_hardware_dispatch(http_server_ctx_t *ctx, struct lws *wsi, int method,
			     const char *uri, int uri_len, char *buf, size_t size,
			     int *status);

#ifdef __cplusplus
}
#endif

#endif /* SHELLCLAW_GATEWAY_ROUTES_HARDWARE_H */
