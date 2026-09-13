/**
 * @file http.c
 * @brief HTTP server lifecycle (libwebsockets). Routes and callbacks live in routes.c / http_lws.c.
 */
#define _POSIX_C_SOURCE 200809L

#include "gateway/http.h"
#include "gateway/http_lws.h"
#include "gateway/ws.h"
#include "providers/provider.h"
#include "cJSON.h"
#include <libwebsockets.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static http_server_ctx_t *g_ctx;

http_server_ctx_t *http_ctx_get(void)
{
	return g_ctx;
}

void http_ctx_set(http_server_ctx_t *ctx)
{
	g_ctx = ctx;
}

static const struct lws_protocols protocols[] = {
	{
		.name = "http",
		.callback = http_callback,
		.rx_buffer_size = RESP_BUF_SIZE,
	},
	{
		.name = "ws",
		.callback = ws_callback,
		.rx_buffer_size = 256,
	},
	{ .name = NULL },
};

static const struct lws_http_mount mount_http = {
	.mountpoint = "/",
	.origin = "http",
	.def = "http",
	.protocol = "http",
	.origin_protocol = LWSMPRO_CALLBACK,
	.mountpoint_len = 1,
	.mount_next = NULL,
};

static const struct lws_http_mount mount_ws = {
	.mountpoint = "/ws",
	.origin = "ws",
	.def = "ws",
	.protocol = "ws",
	.origin_protocol = LWSMPRO_CALLBACK,
	.mountpoint_len = 3,
	.mount_next = &mount_http,
};

void http_emit_ws_provider_status(void)
{
	char *status_json;
	cJSON *root;
	cJSON *type_item;
	char *frame;
	if (!g_ctx) return;
	status_json = provider_router_api_status_json();
	if (!status_json) return;
	root = cJSON_Parse(status_json);
	free(status_json);
	if (!root) return;
	type_item = cJSON_CreateString("provider_status");
	if (!type_item) {
		cJSON_Delete(root);
		return;
	}
	cJSON_AddItemToObject(root, "type", type_item);
	frame = cJSON_PrintUnformatted(root);
	cJSON_Delete(root);
	if (!frame) return;
	ws_broadcast_text(frame);
	free(frame);
}

void http_set_live_config(const config_t *cfg)
{
	if (!cfg)
		return;
	if (!g_ctx)
		return;
	g_ctx->cfg = cfg;
}

static void *http_thread_fn(void *arg)
{
	http_server_ctx_t *ctx = arg;
	while (ctx->running && ctx->lws_ctx)
		lws_service(ctx->lws_ctx, 50);
	return NULL;
}

static int gateway_host_is_bind_all(const char *host)
{
	if (!host)
		return 0;
	return strcmp(host, "0.0.0.0") == 0 || strcmp(host, "*") == 0;
}

int http_start(const config_t *cfg, struct auth_ctx *auth_ctx, const char *config_path)
{
	const char *host;
	int bind_all;
	http_server_ctx_t *ctx;
	struct lws_context_creation_info info;

	if (!cfg || !auth_ctx || g_ctx) return -1;
	host = config_gateway_host(cfg);
	bind_all = gateway_host_is_bind_all(host);
	if (bind_all && !config_gateway_allow_bind_all(cfg))
		return -1;
	ctx = calloc(1, sizeof(*ctx));
	if (!ctx) return -1;
	ctx->cfg = cfg;
	ctx->auth = auth_ctx;
	ctx->config_path = config_path ? strdup(config_path) : NULL;
	ctx->start_time = time(NULL);
	ctx->running = 1;
	memset(&info, 0, sizeof(info));
	info.port = config_gateway_port(cfg);
	/* LWS iface NULL = INADDR_ANY. Pass host so 127.0.0.1 is not all-NICs. */
	info.iface = bind_all ? NULL : host;
	info.protocols = protocols;
#if defined(LWS_SERVER_OPTION_HTTP_HEADERS_SECURITY_BEST_PRACTICES_ENFORCE)
	info.options = LWS_SERVER_OPTION_HTTP_HEADERS_SECURITY_BEST_PRACTICES_ENFORCE;
#endif
	info.mounts = &mount_ws;
	ctx->lws_ctx = lws_create_context(&info);
	if (!ctx->lws_ctx) {
		free(ctx->config_path);
		free(ctx);
		return -1;
	}
	ws_set_context(ctx->lws_ctx);
	g_ctx = ctx;
	if (pthread_create(&ctx->thread, NULL, http_thread_fn, ctx) != 0) {
		lws_context_destroy(ctx->lws_ctx);
		free(ctx->config_path);
		free(ctx);
		g_ctx = NULL;
		return -1;
	}
	return 0;
}

void http_stop(void)
{
	provider_router_set_status_changed_callback(NULL);
	if (!g_ctx) return;
	g_ctx->running = 0;
	ws_shutdown_signal();
	if (g_ctx->thread) {
		pthread_join(g_ctx->thread, NULL);
		g_ctx->thread = 0;
	}
	if (g_ctx->lws_ctx) {
		lws_context_destroy(g_ctx->lws_ctx);
		g_ctx->lws_ctx = NULL;
	}
	ws_cleanup();
	free(g_ctx->config_path);
	free(g_ctx);
	g_ctx = NULL;
}
