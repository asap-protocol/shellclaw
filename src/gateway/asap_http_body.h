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
