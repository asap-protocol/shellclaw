/**
 * @file routes.h
 * @brief REST route dispatch and JSON response helpers for the gateway HTTP API.
 */

#ifndef SHELLCLAW_GATEWAY_ROUTES_H
#define SHELLCLAW_GATEWAY_ROUTES_H

#include "gateway/http_lws.h"
#include "gateway/uri_match.h"
#include "cJSON.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void json_error(char *buf, size_t size, int *status, int code, const char *msg);

int json_print_to_buf(cJSON *obj, char *buf, size_t size, int *status);

int dispatch_route(http_server_ctx_t *ctx, struct lws *wsi, int method,
                   const char *uri, int uri_len, const char *body, size_t body_len,
                   char *buf, size_t size, int *status);

#ifdef __cplusplus
}
#endif

#endif /* SHELLCLAW_GATEWAY_ROUTES_H */
