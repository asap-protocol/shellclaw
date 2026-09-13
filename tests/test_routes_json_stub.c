/**
 * @file test_routes_json_stub.c
 * @brief Minimal json_error/json_print_to_buf for routes_hardware unit tests.
 */
#include "cJSON.h"

struct lws;
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void json_response(char *buf, size_t size, int *status, const char *json)
{
	if (!buf || size == 0 || !status)
		return;
	*status = 200;
	{
		size_t len = strlen(json);

		if (len >= size)
			len = size - 1;
		memcpy(buf, json, len);
		buf[len] = '\0';
	}
}

void json_error(char *buf, size_t size, int *status, int code, const char *msg)
{
	cJSON *obj;
	char *s;

	if (!buf || size == 0 || !status)
		return;
	*status = code;
	obj = cJSON_CreateObject();
	if (!obj)
		return;
	cJSON_AddItemToObject(obj, "error", cJSON_CreateString(msg));
	s = cJSON_PrintUnformatted(obj);
	cJSON_Delete(obj);
	if (!s)
		return;
	{
		size_t len = strlen(s);

		if (len >= size)
			len = size - 1;
		memcpy(buf, s, len);
		buf[len] = '\0';
		free(s);
	}
}

const char *http_request_bearer_token(struct lws *wsi, char *buf, size_t buf_size)
{
	(void)wsi;
	(void)buf;
	(void)buf_size;
	return NULL;
}

int json_print_to_buf(cJSON *obj, char *buf, size_t size, int *status)
{
	char *s;

	if (!obj) {
		json_error(buf, size, status, 500, "Internal error");
		return -1;
	}
	s = cJSON_PrintUnformatted(obj);
	if (!s) {
		json_error(buf, size, status, 500, "Internal error");
		return -1;
	}
	json_response(buf, size, status, s);
	free(s);
	return 0;
}
