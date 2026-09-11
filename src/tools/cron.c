/**
 * @file cron.c
 * @brief Cron scheduler: schedule parsing, next_run, cron channel for agent injection.
 */
#define _POSIX_C_SOURCE 200809L

#include "tools/cron.h"
#include "crypto/crypto.h"
#include "core/memory.h"
#include "channels/channel.h"
#include "core/config.h"
#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define CRON_TOOL_PARAMS "{\"type\":\"object\",\"properties\":{\"operation\":{\"type\":\"string\",\"enum\":[\"list\",\"create\",\"delete\",\"toggle\"]},\"id\":{\"type\":\"string\"},\"schedule\":{\"type\":\"string\"},\"message\":{\"type\":\"string\"},\"channel\":{\"type\":\"string\"},\"recipient\":{\"type\":\"string\"}},\"required\":[\"operation\"]}"

#define CRON_PREFIX_INTERVAL "interval:"
#define CRON_PREFIX_AT       "at:"
#define CRON_PREFIX_CRON     "cron:"
#define CRON_MAX_ITER_MINUTES (8 * 24 * 60)

static int parse_field(const char *s, int *out, int min_val, int max_val)
{
	if (!s || !out) return -1;
	if (strcmp(s, "*") == 0) {
		*out = -1;
		return 0;
	}
	char *end = NULL;
	long n = strtol(s, &end, 10);
	if (end == s || n < min_val || n > max_val) return -1;
	*out = (int)n;
	return 0;
}

static int parse_range(const char *s, int *lo, int *hi, int min_val, int max_val)
{
	if (!s || !lo || !hi) return -1;
	const char *dash = strchr(s, '-');
	if (!dash) {
		*hi = -1;
		return parse_field(s, lo, min_val, max_val);
	}
	char part[32];
	size_t n = (size_t)(dash - s);
	if (n >= sizeof(part)) return -1;
	memcpy(part, s, n);
	part[n] = '\0';
	if (parse_field(part, lo, min_val, max_val) != 0) return -1;
	if (parse_field(dash + 1, hi, min_val, max_val) != 0) return -1;
	if (*hi == -1) *hi = *lo;
	return 0;
}

static int field_matches(int val, int lo, int hi)
{
	if (lo == -1) return 1;
	if (hi == -1) return val == lo;
	return val >= lo && val <= hi;
}

static int cron_expr_matches(const int fields[10], int min, int hour, int mday, int mon, int wday)
{
	return field_matches(min, fields[0], fields[1]) &&
	       field_matches(hour, fields[2], fields[3]) &&
	       field_matches(mday, fields[4], fields[5]) &&
	       field_matches(mon, fields[6], fields[7]) &&
	       field_matches(wday, fields[8], fields[9]);
}

static int parse_cron_expr(const char *s, int fields[10])
{
	char buf[256];
	if (!s || strlen(s) >= sizeof(buf)) return -1;
	strncpy(buf, s, sizeof(buf) - 1);
	buf[sizeof(buf) - 1] = '\0';
	char *ctx = NULL;
	const char *tokens[5];
	int n = 0;
	for (char *p = strtok_r(buf, " \t", &ctx); p && n < 5; p = strtok_r(NULL, " \t", &ctx))
		tokens[n++] = p;
	if (n != 5) return -1;
	int lo, hi;
	if (parse_range(tokens[0], &lo, &hi, 0, 59) != 0) return -1;
	fields[0] = lo; fields[1] = hi;
	if (parse_range(tokens[1], &lo, &hi, 0, 23) != 0) return -1;
	fields[2] = lo; fields[3] = hi;
	if (parse_range(tokens[2], &lo, &hi, 1, 31) != 0) return -1;
	fields[4] = lo; fields[5] = hi;
	if (parse_range(tokens[3], &lo, &hi, 1, 12) != 0) return -1;
	fields[6] = lo; fields[7] = hi;
	if (parse_range(tokens[4], &lo, &hi, 0, 7) != 0) return -1;
	if (lo == 7) lo = 0;
	if (hi == 7) hi = 0;
	fields[8] = lo; fields[9] = hi;
	return 0;
}

static long long cron_next_from_expr(const char *cron_part, long long now)
{
	int fields[10];
	if (parse_cron_expr(cron_part, fields) != 0) return -1;
	time_t t = (time_t)now;
	struct tm tm;
	if (!localtime_r(&t, &tm)) return -1;
	int min = tm.tm_min, hour = tm.tm_hour, mday = tm.tm_mday, mon = tm.tm_mon + 1, wday = tm.tm_wday;
	for (int i = 0; i < CRON_MAX_ITER_MINUTES; i++) {
		if (cron_expr_matches(fields, min, hour, mday, mon, wday))
			return (long long)t;
		t += 60;
		if (!localtime_r(&t, &tm)) return -1;
		min = tm.tm_min; hour = tm.tm_hour; mday = tm.tm_mday; mon = tm.tm_mon + 1; wday = tm.tm_wday;
	}
	return -1;
}

int cron_parse_next_run(const char *schedule, long long now, long long *next_out)
{
	if (!schedule || !next_out) return -1;
	if (strncmp(schedule, CRON_PREFIX_INTERVAL, strlen(CRON_PREFIX_INTERVAL)) == 0) {
		long sec = strtol(schedule + strlen(CRON_PREFIX_INTERVAL), NULL, 10);
		if (sec <= 0) return -1;
		*next_out = now + sec;
		return 0;
	}
	if (strncmp(schedule, CRON_PREFIX_AT, strlen(CRON_PREFIX_AT)) == 0) {
		long long ts = (long long)strtoll(schedule + strlen(CRON_PREFIX_AT), NULL, 10);
		*next_out = ts;
		return 0;
	}
	const char *cron_part = schedule;
	if (strncmp(schedule, CRON_PREFIX_CRON, strlen(CRON_PREFIX_CRON)) == 0)
		cron_part = schedule + strlen(CRON_PREFIX_CRON);
	long long next = cron_next_from_expr(cron_part, now);
	if (next < 0) return -1;
	*next_out = next;
	return 0;
}

int cron_is_one_shot(const char *schedule)
{
	if (!schedule) return 0;
	return strncmp(schedule, CRON_PREFIX_AT, strlen(CRON_PREFIX_AT)) == 0;
}

int cron_create_job(const char *schedule, const char *message,
                    const char *channel, const char *recipient,
                    char *id_out, size_t id_size)
{
	if (!schedule || !message || !id_out || id_size == 0) return -1;
	const char *ch = channel ? channel : "cli";
	const char *rec = recipient ? recipient : "default";
	if (id_out[0] == '\0') {
		unsigned char rnd[2];
		if (crypto_read_urandom(rnd, sizeof(rnd)) != 0) return -1;
		snprintf(id_out, id_size, "cron_%ld_%02x%02x", (long)time(NULL), rnd[0], rnd[1]);
	}
	long long now = (long long)time(NULL);
	long long next = 0;
	if (cron_parse_next_run(schedule, now, &next) != 0) return -1;
	return cron_job_create(id_out, schedule, message, ch, rec, next, 1);
}

static int cron_init(const config_t *cfg)
{
	(void)cfg;
	return 0;
}

static int cron_poll(channel_incoming_msg_t *out, int timeout_ms)
{
	if (!out) return -1;
	(void)timeout_ms;
	long long now = (long long)time(NULL);
	cron_job_row_t row;
	memset(&row, 0, sizeof(row));
	if (cron_job_get_next_due(now, &row) != 1) return 0;
	int is_one_shot = cron_is_one_shot(row.schedule);
	if (is_one_shot) {
		cron_job_delete(row.id);
	} else {
		long long next = 0;
		if (cron_parse_next_run(row.schedule, now, &next) == 0)
			cron_job_update_next_run(row.id, next);
	}
	memset(out, 0, sizeof(*out));
	char session_id[256];
	snprintf(session_id, sizeof(session_id), "%s:%s",
		row.channel[0] ? row.channel : "cli",
		row.recipient[0] ? row.recipient : "default");
	out->session_id = strdup(session_id);
	out->user_id = strdup(row.id);
	out->text = strdup(row.message ? row.message : "");
	out->attachments = NULL;
	out->attachments_count = 0;
	cron_job_row_free(&row);
	return 1;
}

static int cron_send(const char *recipient, const char *text,
                    const channel_attachment_t *attachments, size_t att_count)
{
	if (!recipient || !text) return -1;
	const char *colon = strchr(recipient, ':');
	if (!colon) return -1;
	char ch_name[64];
	size_t n = (size_t)(colon - recipient);
	if (n >= sizeof(ch_name)) return -1;
	memcpy(ch_name, recipient, n);
	ch_name[n] = '\0';
	const channel_t *ch = channel_get_by_name(ch_name);
	if (!ch || !ch->send) return -1;
	return ch->send(colon + 1, text, attachments, att_count);
}

static void cron_cleanup(void)
{
}

static const channel_t cron_channel = {
	.name = "cron",
	.init = cron_init,
	.poll = cron_poll,
	.send = cron_send,
	.cleanup = cron_cleanup,
};

const channel_t *channel_cron_get(void)
{
	return &cron_channel;
}

static int cron_tool_execute(const char *args_json, char *result_buf, size_t max_len)
{
	if (!args_json || !result_buf || max_len == 0) return -1;
	cJSON *root = cJSON_Parse(args_json);
	if (!root) {
		snprintf(result_buf, max_len, "{\"error\":\"invalid JSON\"}");
		return -1;
	}
	cJSON *op = cJSON_GetObjectItem(root, "operation");
	if (!op || !cJSON_IsString(op)) {
		cJSON_Delete(root);
		snprintf(result_buf, max_len, "{\"error\":\"operation required\"}");
		return -1;
	}
	const char *operation = op->valuestring;
	int ret = 0;
	if (strcmp(operation, "list") == 0) {
		cron_job_row_t rows[64];
		memset(rows, 0, sizeof(rows));
		int n = cron_job_list(rows, 64);
		if (n < 0) {
			cJSON_Delete(root);
			snprintf(result_buf, max_len, "{\"error\":\"failed to list jobs\"}");
			return -1;
		}
		cJSON *arr = cJSON_CreateArray();
		if (!arr) {
			for (int i = 0; i < n; i++) cron_job_row_free(&rows[i]);
			cJSON_Delete(root);
			snprintf(result_buf, max_len, "{\"error\":\"out of memory\"}");
			return -1;
		}
		for (int i = 0; i < n; i++) {
			cJSON *obj = cJSON_CreateObject();
			if (!obj) break;
			cJSON_AddItemToObject(obj, "id", cJSON_CreateString(rows[i].id));
			cJSON_AddItemToObject(obj, "schedule", cJSON_CreateString(rows[i].schedule ? rows[i].schedule : ""));
			cJSON_AddItemToObject(obj, "message", cJSON_CreateString(rows[i].message ? rows[i].message : ""));
			cJSON_AddItemToObject(obj, "channel", cJSON_CreateString(rows[i].channel));
			cJSON_AddItemToObject(obj, "recipient", cJSON_CreateString(rows[i].recipient));
			cJSON_AddItemToObject(obj, "next_run", cJSON_CreateNumber((double)rows[i].next_run));
			cJSON_AddItemToObject(obj, "enabled", cJSON_CreateBool(rows[i].enabled));
			cJSON_AddItemToArray(arr, obj);
		}
		for (int i = 0; i < n; i++) cron_job_row_free(&rows[i]);
		char *s = cJSON_PrintUnformatted(arr);
		cJSON_Delete(arr);
		if (s) {
			snprintf(result_buf, max_len, "%s", s);
			free(s);
		} else {
			snprintf(result_buf, max_len, "{\"error\":\"serialization failed\"}");
			ret = -1;
		}
	} else if (strcmp(operation, "create") == 0) {
		cJSON *schedule = cJSON_GetObjectItem(root, "schedule");
		cJSON *message = cJSON_GetObjectItem(root, "message");
		if (!cJSON_IsString(schedule) || !cJSON_IsString(message)) {
			cJSON_Delete(root);
			snprintf(result_buf, max_len, "{\"error\":\"schedule and message required\"}");
			return -1;
		}
		cJSON *id_node = cJSON_GetObjectItem(root, "id");
		cJSON *channel = cJSON_GetObjectItem(root, "channel");
		cJSON *recipient = cJSON_GetObjectItem(root, "recipient");
		char id[128];
		if (cJSON_IsString(id_node) && id_node->valuestring[0])
			snprintf(id, sizeof(id), "%.127s", id_node->valuestring);
		else
			id[0] = '\0';
		const char *ch = (channel && cJSON_IsString(channel)) ? channel->valuestring : NULL;
		const char *rec = (recipient && cJSON_IsString(recipient)) ? recipient->valuestring : NULL;
		if (cron_create_job(schedule->valuestring, message->valuestring, ch, rec, id, sizeof(id)) != 0) {
			cJSON_Delete(root);
			snprintf(result_buf, max_len, "{\"error\":\"failed to create job\"}");
			return -1;
		}
		snprintf(result_buf, max_len, "{\"ok\":true,\"id\":\"%s\"}", id);
	} else if (strcmp(operation, "delete") == 0) {
		cJSON *id_node = cJSON_GetObjectItem(root, "id");
		if (!cJSON_IsString(id_node) || !id_node->valuestring[0]) {
			cJSON_Delete(root);
			snprintf(result_buf, max_len, "{\"error\":\"id required\"}");
			return -1;
		}
		if (cron_job_delete(id_node->valuestring) != 0) {
			cJSON_Delete(root);
			snprintf(result_buf, max_len, "{\"error\":\"job not found\"}");
			return -1;
		}
		snprintf(result_buf, max_len, "{\"ok\":true}");
	} else if (strcmp(operation, "toggle") == 0) {
		cJSON *id_node = cJSON_GetObjectItem(root, "id");
		if (!cJSON_IsString(id_node) || !id_node->valuestring[0]) {
			cJSON_Delete(root);
			snprintf(result_buf, max_len, "{\"error\":\"id required\"}");
			return -1;
		}
		if (cron_job_toggle(id_node->valuestring) != 0) {
			cJSON_Delete(root);
			snprintf(result_buf, max_len, "{\"error\":\"job not found\"}");
			return -1;
		}
		snprintf(result_buf, max_len, "{\"ok\":true}");
	} else {
		cJSON_Delete(root);
		snprintf(result_buf, max_len, "{\"error\":\"invalid operation\"}");
		return -1;
	}
	cJSON_Delete(root);
	return ret;
}

static const tool_t CRON_TOOL = {
	.name = "cron",
	.description = "Manage scheduled jobs: list, create (schedule+message+channel+recipient), delete, toggle. Schedule formats: interval:N (every N sec), at:unix_ts (one-shot), cron:min hour dom month dow.",
	.parameters_json = CRON_TOOL_PARAMS,
	.execute = cron_tool_execute,
};

const tool_t *tool_cron_get(void)
{
	return &CRON_TOOL;
}
