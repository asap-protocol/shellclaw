/**
 * @file asap_http_body.h
 * @brief Request body buffer for POST /asap and default static buffer for other routes.
 */
#ifndef SHELLCLAW_GATEWAY_ASAP_HTTP_BODY_H
#define SHELLCLAW_GATEWAY_ASAP_HTTP_BODY_H

#include "gateway/http_lws.h"
#include <stddef.h>

struct lws;

#ifdef __cplusplus
extern "C" {
#endif

typedef struct asap_http_body {
	char body[BODY_BUF_SIZE];
	size_t body_len;
	char *body_dyn;
	size_t body_dyn_len;
	size_t body_dyn_cap;
	int use_dyn_body;
	int body_too_large;
} asap_http_body_t;

/**
 * Parse HTTP Content-Length for POST /asap (strict decimal, no junk).
 * @return 0 on success, -1 on invalid or out of range.
 */
int asap_http_body_parse_content_length(const char *cl_buf, long *cl_out);

/**
 * True when Content-Length exceeds the static POST/PUT cap (BODY_BUF_SIZE).
 * POST /asap uses a 1 MiB dynamic buffer (use_dyn_body); skip the static cap
 * so envelopes between 64 KiB and ASAP_BODY_MAX are not 413'd.
 * A NULL body pointer is fail-closed (returns 1).
 *
 * Example: asap_http_body_exceeds_static_cap(&body, 70000) is 0 when
 * body.use_dyn_body is set, and 1 for the default static buffer.
 *
 * @return 1 if the static cap applies and is exceeded, or body is NULL; 0 otherwise.
 */
int asap_http_body_exceeds_static_cap(const asap_http_body_t *body, long content_length);

/**
 * For POST /asap: validate Content-Length and allocate dynamic buffer.
 * @return 0 ok, -1 body too large, -2 allocation failure; non-/asap returns 0.
 */
int asap_http_body_init_from_request(struct lws *wsi, asap_http_body_t *body);

void asap_http_body_append(asap_http_body_t *body, const void *in, size_t len);
void asap_http_body_free(asap_http_body_t *body);

#ifdef __cplusplus
}
#endif

#endif /* SHELLCLAW_GATEWAY_ASAP_HTTP_BODY_H */
