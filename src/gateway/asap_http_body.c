/**
 * @file asap_http_body.c
 * @brief Dynamic POST /asap body handling for libwebsockets HTTP callbacks.
 */
#define _POSIX_C_SOURCE 200809L

#include "gateway/asap_http_body.h"
#include "gateway/uri_match.h"
#include <brotli/decode.h>
#include <errno.h>
#include <libwebsockets.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <zlib.h>

static int asap_is_asap_post(struct lws *wsi)
{
	char uri[256];
	int uri_len;

	if (lws_hdr_total_length(wsi, WSI_TOKEN_POST_URI) <= 0)
		return 0;
	uri_len = lws_hdr_copy(wsi, uri, sizeof(uri), WSI_TOKEN_POST_URI);
	if (uri_len <= 0)
		return 0;
	return uri_exact_eq(uri, uri_len, "/asap");
}

int asap_http_body_parse_content_length(const char *cl_buf, long *cl_out)
{
	char *end = NULL;
	long cl;

	if (!cl_out)
		return -1;
	*cl_out = 0;
	if (!cl_buf || cl_buf[0] == '\0')
		return -1;
	errno = 0;
	cl = strtol(cl_buf, &end, 10);
	if (errno != 0 || end == cl_buf || *end != '\0')
		return -1;
	if (cl < 0 || cl > (long)ASAP_BODY_MAX)
		return -1;
	*cl_out = cl;
	return 0;
}

int asap_http_body_exceeds_static_cap(const asap_http_body_t *body, long content_length)
{
	if (!body)
		return 1;
	if (body->use_dyn_body)
		return 0;
	return content_length > (long)BODY_BUF_SIZE;
}

int asap_http_body_init_from_request(struct lws *wsi, asap_http_body_t *body)
{
	char cl_buf[32] = {0};
	long cl = 0;
	int has_cl;

	if (!body || !wsi)
		return -2;
	if (!asap_is_asap_post(wsi))
		return 0;
	has_cl = lws_hdr_copy(wsi, cl_buf, sizeof cl_buf, WSI_TOKEN_HTTP_CONTENT_LENGTH) > 0;
	if (has_cl && asap_http_body_parse_content_length(cl_buf, &cl) != 0)
		return -1;
	{
		size_t cap = (cl > 0) ? (size_t)cl : (size_t)ASAP_BODY_MAX;
		body->body_dyn = malloc(cap + 1);
		if (!body->body_dyn)
			return -2;
		body->body_dyn[0] = '\0';
		body->body_dyn_len = 0;
		body->body_dyn_cap = cap;
		body->use_dyn_body = 1;
		body->body_too_large = 0;
	}
	return 0;
}

static int decode_gzip(const char *body, size_t len, char *decoded, size_t *decoded_len)
{
	z_stream stream = {0};
	int rc = inflateInit2(&stream, MAX_WBITS + 16); /* Require a gzip header. */
	if (rc != Z_OK)
		return 500;
	stream.next_in = (z_const Bytef *)body;
	stream.avail_in = (uInt)len;
	stream.next_out = (Bytef *)decoded;
	stream.avail_out = ASAP_BODY_MAX + 1;
	rc = inflate(&stream, Z_FINISH);
	*decoded_len = stream.total_out;
	int status = 400;
	if (stream.total_out > ASAP_BODY_MAX)
		status = 413;
	else if (rc == Z_STREAM_END && stream.avail_in == 0)
		status = 0;
	else if (rc == Z_MEM_ERROR)
		status = 500;
	inflateEnd(&stream);
	return status;
}

static int decode_brotli(const char *body, size_t len, char *decoded, size_t *decoded_len)
{
	BrotliDecoderState *state = BrotliDecoderCreateInstance(NULL, NULL, NULL);
	if (!state)
		return 500;
	const uint8_t *input = (const uint8_t *)body;
	uint8_t *output = (uint8_t *)decoded;
	size_t space = ASAP_BODY_MAX + 1;
	BrotliDecoderResult result = BrotliDecoderDecompressStream(state,
		&len, &input, &space, &output, decoded_len);
	BrotliDecoderDestroyInstance(state);
	if (*decoded_len > ASAP_BODY_MAX || result == BROTLI_DECODER_RESULT_NEEDS_MORE_OUTPUT)
		return 413;
	return result == BROTLI_DECODER_RESULT_SUCCESS && len == 0 ? 0 : 400;
}

int asap_http_body_decode(struct lws *wsi, const char *body, size_t len,
	char **decoded, size_t *decoded_len)
{
	char encoding[32];
	*decoded = NULL;
	*decoded_len = len;
	if (len > ASAP_BODY_MAX)
		return 413;
	if (!wsi || lws_hdr_total_length(wsi, WSI_TOKEN_HTTP_CONTENT_ENCODING) <= 0)
		return 0;
	if (lws_hdr_copy(wsi, encoding, sizeof encoding, WSI_TOKEN_HTTP_CONTENT_ENCODING) <= 0)
		return 415;
	char *coding = encoding + strspn(encoding, " \t");
	size_t coding_len = strlen(coding);
	while (coding_len > 0 && (coding[coding_len - 1] == ' ' || coding[coding_len - 1] == '\t'))
		coding[--coding_len] = '\0';
	if (strcasecmp(coding, "identity") == 0)
		return 0;
	int gzip = strcasecmp(coding, "gzip") == 0;
	if (!gzip && strcasecmp(coding, "br") != 0)
		return 415;

	char *output = malloc(ASAP_BODY_MAX + 1);
	if (!output)
		return 500;
	int status = gzip ? decode_gzip(body, len, output, decoded_len)
	                  : decode_brotli(body, len, output, decoded_len);
	if (status == 0) {
		output[*decoded_len] = '\0';
		*decoded = output;
	} else {
		free(output);
	}
	return status;
}

void asap_http_body_append(asap_http_body_t *body, const void *in, size_t len)
{
	if (!body || !in || len == 0)
		return;
	if (body->use_dyn_body) {
		size_t remain = body->body_dyn_cap - body->body_dyn_len;
		if (len > remain) {
			body->body_too_large = 1;
			len = remain;
		}
		if (len > 0) {
			memcpy(body->body_dyn + body->body_dyn_len, in, len);
			body->body_dyn_len += len;
			body->body_dyn[body->body_dyn_len] = '\0';
		}
		return;
	}
	{
		size_t remain = BODY_BUF_SIZE - body->body_len - 1;
		size_t n = len;

		if (n > remain) {
			body->body_too_large = 1;
			n = remain;
		}
		if (n > 0) {
			memcpy(body->body + body->body_len, in, n);
			body->body_len += n;
			body->body[body->body_len] = '\0';
		}
	}
}

void asap_http_body_free(asap_http_body_t *body)
{
	if (!body)
		return;
	if (body->body_dyn) {
		free(body->body_dyn);
		body->body_dyn = NULL;
	}
	body->body_dyn_len = 0;
	body->body_dyn_cap = 0;
	body->use_dyn_body = 0;
	body->body_too_large = 0;
	body->body_len = 0;
	body->body[0] = '\0';
}
