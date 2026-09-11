/**
 * @file http_lws.h
 * @brief libwebsockets HTTP/WebSocket callbacks and shared gateway server context.
 */

#ifndef SHELLCLAW_GATEWAY_HTTP_LWS_H
#define SHELLCLAW_GATEWAY_HTTP_LWS_H

#include "core/config.h"
#include "core/version.h"
#include "gateway/auth.h"
#include "gateway/lws_compat.h"
#include <libwebsockets.h>
#include <pthread.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GATEWAY_VERSION SHELLCLAW_RELEASE_VERSION
#define RESP_BUF_SIZE 65536
#define LWS_HEADER_SPACE 2048
#define CONFIG_BODY_MAX 65536
#define BODY_BUF_SIZE CONFIG_BODY_MAX
#define ASAP_BODY_MAX (1024 * 1024)

enum http_method {
	HTTP_GET = 1,
	HTTP_POST = 2,
	HTTP_PUT = 4,
	HTTP_DELETE = 5
};

typedef struct http_server_ctx {
	const config_t *cfg;
	auth_ctx_t *auth;
	char *config_path;
	time_t start_time;
	struct lws_context *lws_ctx;
	pthread_t thread;
	volatile int running;
} http_server_ctx_t;

/** Global server context (set by http_start, cleared by http_stop). */
http_server_ctx_t *http_ctx_get(void);
void http_ctx_set(http_server_ctx_t *ctx);

int http_callback(struct lws *wsi, enum lws_callback_reasons reason, void *user,
                  void *in, size_t len);
int ws_callback(struct lws *wsi, enum lws_callback_reasons reason, void *user,
                void *in, size_t len);

/**
 * Extract Bearer token from HTTP headers into @p buf.
 * @return Pointer into @p buf past "Bearer ", or NULL if missing/invalid.
 */
const char *http_request_bearer_token(struct lws *wsi, char *buf, size_t buf_size);

#ifdef __cplusplus
}
#endif

#endif /* SHELLCLAW_GATEWAY_HTTP_LWS_H */
