/**
 * @file context_http.c
 * @brief HTTP fetch helpers for get_context (libcurl).
 */
#define _POSIX_C_SOURCE 200809L

#include "tools/context_internal.h"
#include <curl/curl.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

struct wrbuf {
	char *p;
	size_t n;
};

static size_t curl_cb(char *chunk, size_t sz, size_t nm, void *ud)
{
	struct wrbuf *b = (struct wrbuf *)ud;
	size_t add;
	if (!b || !b->p || nm == 0)
		return 0;
	if (sz > SIZE_MAX / nm)
		return 0;
	add = sz * nm;
	if (add > CTX_RESP_MAX - b->n)
		return 0;
	memcpy(b->p + b->n, chunk, add);
	b->n += add;
	b->p[b->n] = '\0';
	return add;
}

#ifdef SHELLCLAW_CONTEXT_TEST
static int fake_http(const char *url, long *code, char **body)
{
	const char *s = NULL;
	if (!url || !code || !body)
		return 0;
	if (strstr(url, "ip-api.com"))
		s = ctx_g_fake.geo;
	else if (strstr(url, "open-meteo.com"))
		s = ctx_g_fake.wx;
	else if (strstr(url, "date.nager.at"))
		s = ctx_g_fake.hy;
	if (!s)
		return 0;
	if (strcmp(s, "__CTX_HTTP_FAIL__") == 0) {
		*code = 503L;
		*body = NULL;
		return 1;
	}
	if (strstr(s, "\"status\":\"fail\""))
		*code = 502L;
	else
		*code = 200L;
	*body = strdup(s);
	return (*body != NULL) ? 1 : 0;
}
#endif

int ctx_fetch_url(const char *url, long *http_code, char **body)
{
	CURL *c;
	struct wrbuf w;
	long code = 0;
	CURLcode r;
	if (!url || !http_code || !body)
		return -1;
	*body = NULL;
#ifdef SHELLCLAW_CONTEXT_TEST
	if (fake_http(url, &code, body)) {
		if (*body != NULL) {
			*http_code = code;
			return 0;
		}
		return -1;
	}
#endif
	w.p = calloc(1u, CTX_RESP_MAX);
	w.n = 0;
	if (!w.p)
		return -1;
	c = curl_easy_init();
	if (!c) {
		free(w.p);
		return -1;
	}
	curl_easy_setopt(c, CURLOPT_URL, url);
	curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
	curl_easy_setopt(c, CURLOPT_TIMEOUT, CTX_CURL_TO_SEC);
	curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, CTX_CURL_TO_SEC);
	curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, curl_cb);
	curl_easy_setopt(c, CURLOPT_WRITEDATA, &w);
	curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 1L);
	curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 2L);
	r = curl_easy_perform(c);
	curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
	curl_easy_cleanup(c);
	if (r != CURLE_OK || code < 100 || w.n == 0u) {
		free(w.p);
		return -1;
	}
	*http_code = code;
	*body = w.p;
	return 0;
}
