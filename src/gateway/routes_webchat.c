/**
 * @file routes_webchat.c
 * @brief WebChat history and message receipts for an authenticated device.
 */
#define _POSIX_C_SOURCE 200809L

#include "core/memory.h"
#include "gateway/routes.h"
#include "gateway/routes_webchat.h"
#include "providers/provider.h"
#include "tweetnacl.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int webchat_request_session(struct lws *wsi, char session[WEBCHAT_SESSION_SIZE])
{
	char auth[256];
	const char *token = wsi ? http_request_bearer_token(wsi, auth, sizeof(auth)) : NULL;
	if (!token) return -1;
	unsigned char digest[crypto_hash_BYTES];
	if (crypto_hash(digest, (const unsigned char *)token, strlen(token)) != 0) return -1;
	memcpy(session, WEBCHAT_DEVICE_PREFIX, sizeof(WEBCHAT_DEVICE_PREFIX));
	for (size_t i = 0; i < 32; i++)
		snprintf(session + sizeof(WEBCHAT_DEVICE_PREFIX) - 1 + 2 * i, 3, "%02x", digest[i]);
	return 0;
}

static void handle_history(struct lws *wsi, const char *session, char *buf, size_t size, int *status)
{
	char value[32];
	int64_t after = 0;
	const char *cursor = lws_get_urlarg_by_name(wsi, "after=", value, sizeof(value));
	if (cursor) {
		char *end;
		errno = 0;
		after = strtoll(cursor, &end, 10);
		if (errno || end == cursor || *end || after < 0) {
			json_error(buf, size, status, 400, "Invalid history cursor");
			return;
		}
	}
	if (webchat_store_history(session, after, buf, size) != 0)
		json_error(buf, size, status, 500, "Cannot read the conversation");
	else
		*status = 200;
}

static int valid_id(const char *id)
{
	return id && strlen(id) == 32 && strspn(id, "0123456789abcdef") == 32;
}

static void handle_send(const char *session, const char *body, size_t body_len,
                        char *buf, size_t size, int *status)
{
	cJSON *obj = body && body_len && !memchr(body, '\0', body_len)
		? cJSON_ParseWithLengthOpts(body, body_len + 1, NULL, 1) : NULL;
	const char *id = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(obj, "id"));
	const char *text = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(obj, "text"));
	if (!valid_id(id) || !text || provider_text_is_blank(text) || strlen(text) > WS_TEXT_MAX) {
		json_error(buf, size, status, 400, "Invalid chat message");
	} else {
		int rc = webchat_store_accept(session, id, text);
		if (rc == 1)
			json_error(buf, size, status, 409, "The message ID is already in use");
		else if (rc != 0)
			json_error(buf, size, status, 500, "Cannot save the message");
		else {
			snprintf(buf, size, "{\"accepted\":true}");
			*status = 200;
		}
	}
	cJSON_Delete(obj);
}

void handle_webchat(struct lws *wsi, int method, const char *body, size_t body_len,
                    char *buf, size_t size, int *status)
{
	char session[WEBCHAT_SESSION_SIZE];
	if (webchat_request_session(wsi, session) != 0)
		json_error(buf, size, status, 401, "Unauthorized");
	else if (method == HTTP_GET)
		handle_history(wsi, session, buf, size, status);
	else if (method == HTTP_POST)
		handle_send(session, body, body_len, buf, size, status);
	else
		json_error(buf, size, status, 405, "Method not allowed");
}
