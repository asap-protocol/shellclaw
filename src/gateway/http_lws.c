/**
 * @file http_lws.c
 * @brief libwebsockets HTTP and WebSocket callbacks (Bearer / subprotocol auth).
 */
#define _POSIX_C_SOURCE 200809L

#include "gateway/http_lws.h"
#include "gateway/asap_http_body.h"
#include "gateway/routes.h"
#include "gateway/auth.h"
#include "gateway/uri_match.h"
#include "gateway/static.h"
#include "gateway/ws.h"
#include "cJSON.h"
#include <libwebsockets.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void http_lws_tx_completed(struct lws *wsi)
{
	if (lws_http_transaction_completed(wsi) != 0)
		return;
}

typedef struct http_conn {
	char *response;
	size_t response_len;
	size_t response_sent;
	int headers_sent;
	int status;
	int has_body;
	int is_static;
	const unsigned char *static_data;
	size_t static_len;
	const char *static_content_type;
	size_t static_sent;
	asap_http_body_t body;
	int body_dispatched;
} http_conn_t;

static int http_parse_method(struct lws *wsi)
{
	/* Detect method by checking which URI-method header token is present.
	 * lws_hdr_total_length returns the length without copying, which
	 * avoids the bug where lws_hdr_copy returns 0 when the destination
	 * buffer is too small to hold the URI (breaks routes with long paths
	 * like /api/skills, /api/cron, etc.). */
	if (lws_hdr_total_length(wsi, WSI_TOKEN_POST_URI) > 0)
		return HTTP_POST;
	if (lws_hdr_total_length(wsi, WSI_TOKEN_PUT_URI) > 0)
		return HTTP_PUT;
	if (lws_hdr_total_length(wsi, WSI_TOKEN_DELETE_URI) > 0)
		return HTTP_DELETE;
	if (lws_hdr_total_length(wsi, WSI_TOKEN_PATCH_URI) > 0)
		return HTTP_PUT;
	/* HTTP/2 / fallbacks: ask for the explicit :method pseudo-header. */
	char meth_buf[16] = {0};
	if (lws_hdr_custom_copy(wsi, meth_buf, sizeof(meth_buf), ":method", 7) > 0 ||
	    lws_hdr_copy(wsi, meth_buf, sizeof(meth_buf), WSI_TOKEN_HTTP_COLON_METHOD) > 0) {
		if (strcmp(meth_buf, "POST") == 0) return HTTP_POST;
		if (strcmp(meth_buf, "PUT") == 0) return HTTP_PUT;
		if (strcmp(meth_buf, "DELETE") == 0) return HTTP_DELETE;
		if (strcmp(meth_buf, "PATCH") == 0) return HTTP_PUT;
	}
	return HTTP_GET;
}

static int http_body_content_length(struct lws *wsi, long *cl_out)
{
	char cl_buf[32] = {0};
	if (!cl_out || lws_hdr_copy(wsi, cl_buf, sizeof cl_buf,
	                           WSI_TOKEN_HTTP_CONTENT_LENGTH) <= 0)
		return -1;
	*cl_out = atol(cl_buf);
	return 0;
}

static int http_copy_request_uri(struct lws *wsi, char *uri, int uri_size)
{
	int n;
	n = lws_hdr_copy(wsi, uri, uri_size, WSI_TOKEN_GET_URI);
	if (n > 0) return n;
	n = lws_hdr_copy(wsi, uri, uri_size, WSI_TOKEN_POST_URI);
	if (n > 0) return n;
	n = lws_hdr_copy(wsi, uri, uri_size, WSI_TOKEN_PUT_URI);
	if (n > 0) return n;
	n = lws_hdr_copy(wsi, uri, uri_size, WSI_TOKEN_DELETE_URI);
	if (n > 0) return n;
	n = lws_hdr_copy(wsi, uri, uri_size, WSI_TOKEN_PATCH_URI);
	return n;
}

static void http_dispatch_body(http_server_ctx_t *ctx, struct lws *wsi, http_conn_t *conn)
{
	char uri[256];
	int uri_len;
	int method;
	if (!conn || !conn->has_body || conn->body_dispatched)
		return;
	conn->body_dispatched = 1;
	uri_len = http_copy_request_uri(wsi, uri, sizeof(uri));
	method = http_parse_method(wsi);
	if (conn->body.body_too_large) {
		json_error(conn->response, RESP_BUF_SIZE, &conn->status, 413,
		           "Request body too large");
	} else if (conn->body.use_dyn_body) {
		dispatch_route(ctx, wsi, method, uri, uri_len,
		               conn->body.body_dyn, conn->body.body_dyn_len,
		               conn->response, RESP_BUF_SIZE, &conn->status);
	} else {
		dispatch_route(ctx, wsi, method, uri, uri_len,
		               conn->body.body, conn->body.body_len,
		               conn->response, RESP_BUF_SIZE, &conn->status);
	}
	conn->response_len = strlen(conn->response);
	conn->has_body = 0;
}

const char *http_request_bearer_token(struct lws *wsi, char *buf, size_t buf_size)
{
	int n = lws_hdr_copy(wsi, buf, (int)buf_size, WSI_TOKEN_HTTP_AUTHORIZATION);
	if (n <= 0)
		n = lws_hdr_custom_copy(wsi, buf, (int)buf_size, "authorization", 13);
	if (n <= 0)
		return NULL;
	if (n < 8 || strncmp(buf, "Bearer ", 7) != 0)
		return NULL;
	return buf + 7;
}

/** WebSocket upgrade: Bearer from Authorization (or custom header); browser may send bearer.<token> subprotocol. */
static int ws_copy_upgrade_token(struct lws *wsi, char *token_out, size_t token_size)
{
	char auth_buf[256];
	const char *token;
	if (!wsi || !token_out || token_size == 0) return -1;
	token_out[0] = '\0';
	token = http_request_bearer_token(wsi, auth_buf, sizeof(auth_buf));
	if (!token) {
		int n = lws_hdr_custom_copy(wsi, auth_buf, sizeof(auth_buf), "authorization", 13);
		if (n > 7 && strncmp(auth_buf, "Bearer ", 7) == 0)
			token = auth_buf + 7;
	}
	if (token && token[0] != '\0') {
		size_t len = strlen(token);
		if (len >= token_size) len = token_size - 1;
		memcpy(token_out, token, len);
		token_out[len] = '\0';
		return 0;
	}
	{
		char proto[512];
		int n = lws_hdr_copy(wsi, proto, sizeof(proto), WSI_TOKEN_COLON_PROTOCOL);
		if (n <= 0)
			n = lws_hdr_custom_copy(wsi, proto, sizeof(proto), "sec-websocket-protocol", 22);
		if (n > 7 && strncmp(proto, "bearer.", 7) == 0) {
			const char *sub = proto + 7;
			size_t len = (size_t)(n - 7);
			if (len >= token_size) len = token_size - 1;
			memcpy(token_out, sub, len);
			token_out[len] = '\0';
			return token_out[0] != '\0' ? 0 : -1;
		}
	}
	return -1;
}

static int is_static_path(const char *uri, int uri_len, int method)
{
	if (method != HTTP_GET) return 0;
	if (uri_exact_eq(uri, uri_len, "/health")) return 0;
	if (uri_exact_eq(uri, uri_len, "/pair")) return 0;
	if (uri_has_prefix(uri, uri_len, "/.well-known/")) return 0;
	if (uri_has_prefix(uri, uri_len, "/api/")) return 0;
	return 1;
}

static int requires_auth(const char *uri, int uri_len)
{
	if (uri_exact_eq(uri, uri_len, "/health")) return 0;
	if (uri_exact_eq(uri, uri_len, "/pair")) return 0;
	if (uri_has_prefix(uri, uri_len, "/.well-known/")) return 0;
	if (uri_exact_eq(uri, uri_len, "/")) return 0;
	if (uri_has_prefix(uri, uri_len, "/api/")) return 1;
	return 0;
}
int http_callback(struct lws *wsi, enum lws_callback_reasons reason, void *user,
                         void *in, size_t len)
{
	http_server_ctx_t *ctx = http_ctx_get();
	(void)user;
	if (!ctx) return 0;
	switch (reason) {
	case LWS_CALLBACK_HTTP: {
		char uri[256];
		int uri_len = http_copy_request_uri(wsi, uri, sizeof(uri));
		if (uri_len <= 0 || uri_len >= (int)sizeof(uri)) {
			lws_return_http_status(wsi, 400, "Bad request");
			http_lws_tx_completed(wsi);
			return 0;
		}
		int method = http_parse_method(wsi);
		if (requires_auth(uri, uri_len)) {
			char auth_buf[256];
			const char *token = http_request_bearer_token(wsi, auth_buf, sizeof(auth_buf));
			if (!token || !auth_validate_token(ctx->auth, token)) {
				lws_return_http_status(wsi, 401, "{\"error\":\"Unauthorized\"}");
				http_lws_tx_completed(wsi);
				return 0;
			}
		}
		http_conn_t *conn = calloc(1, sizeof(*conn));
		if (!conn) {
			lws_return_http_status(wsi, 500, "Internal error");
			http_lws_tx_completed(wsi);
			return 0;
		}
		conn->response = malloc(RESP_BUF_SIZE);
		if (!conn->response) {
			free(conn);
			lws_return_http_status(wsi, 500, "Internal error");
			http_lws_tx_completed(wsi);
			return 0;
		}
		conn->response[0] = '\0';
		conn->status = 200;
		conn->has_body = (method == HTTP_POST || method == HTTP_PUT);
		/* For /asap POST: enforce 1 MB body cap via Content-Length and allocate
		 * a dynamic buffer so large envelopes are not silently truncated. */
		if (conn->has_body && uri_exact_eq(uri, uri_len, "/asap")) {
			int body_rc = asap_http_body_init_from_request(wsi, &conn->body);
			if (body_rc == -1) {
				free(conn->response);
				free(conn);
				lws_return_http_status(wsi, 413, "{\"error\":\"body too large\"}");
				http_lws_tx_completed(wsi);
				return 0;
			}
			if (body_rc == -2) {
				free(conn->response);
				free(conn);
				lws_return_http_status(wsi, 500, "Internal error");
				http_lws_tx_completed(wsi);
				return 0;
			}
		}
		if (!conn->has_body && is_static_path(uri, uri_len, method)) {
			const unsigned char *data = NULL;
			size_t data_len = 0;
			const char *ct = NULL;
			char path_buf[256];
			if (uri_len >= (int)sizeof(path_buf)) uri_len = (int)sizeof(path_buf) - 1;
			memcpy(path_buf, uri, (size_t)uri_len);
			path_buf[uri_len] = '\0';
			if (static_lookup(path_buf, &data, &data_len, &ct) == 0) {
				conn->is_static = 1;
				conn->static_data = data;
				conn->static_len = data_len;
				conn->static_content_type = ct;
				conn->static_sent = 0;
				free(conn->response);
				conn->response = NULL;
				lws_set_wsi_user(wsi, conn);
				lws_callback_on_writable(wsi);
				return 0;
			}
		}
		if (!conn->has_body) {
			dispatch_route(ctx, wsi, method, uri, uri_len, NULL, 0, conn->response, RESP_BUF_SIZE, &conn->status);
			conn->response_len = strlen(conn->response);
			lws_set_wsi_user(wsi, conn);
			lws_callback_on_writable(wsi);
		} else {
			long cl = 0;
			if (http_body_content_length(wsi, &cl) == 0 && cl > (long)BODY_BUF_SIZE)
				conn->body.body_too_large = 1;
			conn->body.body[0] = '\0';
			conn->body.body_len = 0;
			lws_set_wsi_user(wsi, conn);
			/* In LWS_CALLBACK_HTTP, `in` is the URI string, never the body
			 * (the body arrives via LWS_CALLBACK_HTTP_BODY /
			 * LWS_CALLBACK_HTTP_BODY_COMPLETION). For zero-length POSTs we
			 * still dispatch from BODY_COMPLETION; if it never arrives the
			 * connection closes via LWS_CALLBACK_CLOSED_HTTP. */
			if (conn->body.body_too_large) {
				http_dispatch_body(ctx, wsi, conn);
				lws_callback_on_writable(wsi);
			}
		}
		return 0;
	}
	case LWS_CALLBACK_HTTP_BODY: {
		http_conn_t *conn = lws_wsi_user(wsi);
		long cl;
		size_t received;
		if (!conn || !conn->has_body || !in || len == 0)
			return 0;
		asap_http_body_append(&conn->body, in, len);
		/* On some libwebsockets versions (e.g. 4.3) the
		 * LWS_CALLBACK_HTTP_BODY_COMPLETION callback is not always
		 * delivered for short request bodies. Dispatch eagerly as soon
		 * as we have received Content-Length bytes. */
		received = conn->body.use_dyn_body ? conn->body.body_dyn_len : conn->body.body_len;
		if (http_body_content_length(wsi, &cl) == 0 && cl > 0 &&
		    (long)received >= cl && !conn->body.body_too_large) {
			http_dispatch_body(ctx, wsi, conn);
			lws_callback_on_writable(wsi);
		}
		return 0;
	}
	case LWS_CALLBACK_HTTP_BODY_COMPLETION: {
		http_conn_t *conn = lws_wsi_user(wsi);
		if (conn && conn->has_body) {
			http_dispatch_body(ctx, wsi, conn);
			lws_callback_on_writable(wsi);
		}
		return 0;
	}
	case LWS_CALLBACK_HTTP_WRITEABLE: {
		http_conn_t *conn = lws_wsi_user(wsi);
		if (!conn) return 0;
		if (conn->is_static) {
			if (!conn->headers_sent) {
				unsigned char buf[LWS_PRE + LWS_HEADER_SPACE];
				unsigned char *p = buf + LWS_PRE;
				unsigned char *end = buf + sizeof(buf) - LWS_PRE;
				size_t ct_len = strlen(conn->static_content_type);
				if (lws_add_http_header_status(wsi, 200, &p, end) ||
				    lws_add_http_header_by_token(wsi, WSI_TOKEN_HTTP_CONTENT_TYPE,
				         (unsigned char *)conn->static_content_type, (unsigned int)ct_len, &p, end) ||
				    lws_add_http_header_by_name(wsi, (unsigned char *)"content-encoding",
				         (unsigned char *)"gzip", 4, &p, end) ||
				    lws_add_http_header_content_length(wsi, (lws_filepos_t)conn->static_len, &p, end) ||
				    lws_finalize_http_header(wsi, &p, end))
					return -1;
				int n = (int)(p - (buf + LWS_PRE));
				if (lws_write(wsi, buf + LWS_PRE, (size_t)n, LWS_WRITE_HTTP_HEADERS) != n)
					return -1;
				conn->headers_sent = 1;
			}
			if (conn->static_sent < conn->static_len) {
				size_t to_send = conn->static_len - conn->static_sent;
				if (to_send > 4096) to_send = 4096;
				int m = lws_write(wsi,
				                  (unsigned char *)(conn->static_data + conn->static_sent),
				                  to_send,
				                  conn->static_sent + to_send >= conn->static_len ?
				                  LWS_WRITE_HTTP_FINAL : LWS_WRITE_HTTP);
				if (m < 0) return -1;
				conn->static_sent += (size_t)m;
			}
			if (conn->static_sent >= conn->static_len) {
				free(conn);
				lws_set_wsi_user(wsi, NULL);
				http_lws_tx_completed(wsi);
			}
			return 0;
		}
		if (!conn->response) return 0;
		if (!conn->headers_sent) {
			unsigned char buf[LWS_PRE + LWS_HEADER_SPACE];
			unsigned char *p = buf + LWS_PRE;
			unsigned char *end = buf + sizeof(buf) - LWS_PRE;
			if (lws_add_http_header_status(wsi, (unsigned int)conn->status, &p, end) ||
			    lws_add_http_header_by_token(wsi, WSI_TOKEN_HTTP_CONTENT_TYPE,
			         (unsigned char *)"application/json", 16, &p, end) ||
			    lws_add_http_header_content_length(wsi, (lws_filepos_t)conn->response_len, &p, end) ||
			    lws_finalize_http_header(wsi, &p, end))
				return -1;
			int n = (int)(p - (buf + LWS_PRE));
			if (lws_write(wsi, buf + LWS_PRE, (size_t)n, LWS_WRITE_HTTP_HEADERS) != n)
				return -1;
			conn->headers_sent = 1;
		}
		if (conn->response_sent < conn->response_len) {
			size_t to_send = conn->response_len - conn->response_sent;
			if (to_send > 4096) to_send = 4096;
			int flags = (conn->response_sent + to_send >= conn->response_len) ?
			    LWS_WRITE_HTTP_FINAL : LWS_WRITE_HTTP;
			int m = lws_write(wsi, (unsigned char *)conn->response + conn->response_sent,
			                 to_send, flags);
			if (m < 0) return -1;
			conn->response_sent += (size_t)m;
		}
		if (conn->response_sent >= conn->response_len) {
			free(conn->response);
			asap_http_body_free(&conn->body);
			free(conn);
			lws_set_wsi_user(wsi, NULL);
			http_lws_tx_completed(wsi);
		} else {
			lws_callback_on_writable(wsi);
		}
		return 0;
	}
	case LWS_CALLBACK_CLOSED_HTTP: {
		http_conn_t *conn = lws_wsi_user(wsi);
		if (conn) {
			if (conn->response) free(conn->response);
			asap_http_body_free(&conn->body);
			free(conn);
			lws_set_wsi_user(wsi, NULL);
		}
		return 0;
	}
	default:
		break;
	}
	return 0;
}

int ws_callback(struct lws *wsi, enum lws_callback_reasons reason, void *user,
                      void *in, size_t len)
{
	http_server_ctx_t *ctx = http_ctx_get();
	(void)user;
	switch (reason) {
	case LWS_CALLBACK_HTTP_CONFIRM_UPGRADE: {
		char token_buf[256] = {0};
		if (!ctx || !ctx->auth) return -1;
		if (ws_copy_upgrade_token(wsi, token_buf, sizeof(token_buf)) != 0)
			return -1;
		if (!auth_validate_token(ctx->auth, token_buf))
			return -1;
		return 0;
	}
	case LWS_CALLBACK_ESTABLISHED: {
		int conn_id = ws_next_conn_id();
		lws_set_wsi_user(wsi, (void *)(intptr_t)conn_id);
		if (ws_register_conn(conn_id, (ws_conn_t)wsi) != 0) {
			fprintf(stderr, "shellclaw: ws: connection table full\n");
			return -1;
		}
		break;
	}
	case LWS_CALLBACK_SERVER_WRITEABLE: {
		int conn_id = (int)(intptr_t)lws_wsi_user(wsi);
		if (conn_id <= 0) break;
		char buf[8192];
		size_t len_out = 0;
		if (ws_dequeue_outgoing(conn_id, buf, sizeof(buf), &len_out)) {
			unsigned char frame[LWS_PRE + 8192];
			if (len_out < sizeof(frame) - LWS_PRE) {
				memcpy(frame + LWS_PRE, buf, len_out);
				if (lws_write(wsi, frame + LWS_PRE, len_out, LWS_WRITE_TEXT) < 0)
					break;
			}
			if (ws_has_pending_outgoing(conn_id))
				lws_callback_on_writable(wsi);
		}
		break;
	}
	case LWS_CALLBACK_RECEIVE: {
		int conn_id = (int)(intptr_t)lws_wsi_user(wsi);
		if (conn_id <= 0) break;
		if (len > 0 && in) {
			char *buf = malloc(len + 1);
			if (buf) {
				memcpy(buf, in, len);
				buf[len] = '\0';
				cJSON *root = cJSON_Parse(buf);
				free(buf);
				if (root) {
					cJSON *type = cJSON_GetObjectItem(root, "type");
					cJSON *text = cJSON_GetObjectItem(root, "text");
					if (cJSON_IsString(type) && strcmp(type->valuestring, "message") == 0 &&
					    cJSON_IsString(text) && text->valuestring)
						ws_push_incoming(conn_id, text->valuestring);
					cJSON_Delete(root);
				}
			}
		}
		break;
	}
	case LWS_CALLBACK_CLOSED: {
		int conn_id = (int)(intptr_t)lws_wsi_user(wsi);
		if (conn_id > 0) ws_unregister_conn(conn_id);
		break;
	}
	default:
		break;
	}
	return 0;
}
