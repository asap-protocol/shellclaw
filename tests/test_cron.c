/**
 * @file test_cron.c
 * @brief Unit tests for cron: schedule parsing, next_run, one-shot.
 */

#define _POSIX_C_SOURCE 200809L

#include "tools/cron.h"
#include "core/memory.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define ASSERT(c) do { if (!(c)) { fprintf(stderr, "FAIL: %s:%d %s\n", __FILE__, __LINE__, #c); return 1; } } while (0)
#define RUN(t) do { int r = (t); if (r) return r; } while (0)

static int test_interval_next_run(void)
{
	long long now = 1700000000;
	long long next = 0;
	ASSERT(cron_parse_next_run("interval:3600", now, &next) == 0);
	ASSERT(next == now + 3600);
	ASSERT(cron_parse_next_run("interval:60", now, &next) == 0);
	ASSERT(next == now + 60);
	return 0;
}

static int test_at_one_shot(void)
{
	long long now = 1700000000;
	long long next = 0;
	ASSERT(cron_parse_next_run("at:1700000100", now, &next) == 0);
	ASSERT(next == 1700000100);
	ASSERT(cron_is_one_shot("at:1700000100") == 1);
	ASSERT(cron_is_one_shot("interval:3600") == 0);
	ASSERT(cron_is_one_shot("0 9 * * 1-5") == 0);
	return 0;
}

static int test_cron_expr_next_run(void)
{
	long long now = 1700000000;
	long long next = 0;
	ASSERT(cron_parse_next_run("0 0 * * *", now, &next) == 0);
	ASSERT(next > now);
	return 0;
}

static int test_cron_expr_with_prefix(void)
{
	long long now = 1700000000;
	long long next = 0;
	ASSERT(cron_parse_next_run("cron:0 0 * * *", now, &next) == 0);
	ASSERT(next > now);
	return 0;
}

static int test_cron_expr_skips_current_minute(void)
{
	long long now = 1700000017; /* mid-minute so a same-minute match would be <= now */
	time_t t = (time_t)now;
	struct tm tm;
	ASSERT(localtime_r(&t, &tm) != NULL);
	char schedule[64];
	snprintf(schedule, sizeof(schedule), "cron:%d %d * * *", tm.tm_min, tm.tm_hour);
	long long next = 0;
	ASSERT(cron_parse_next_run(schedule, now, &next) == 0);
	ASSERT(next > now);
	/* Must advance at least to the next minute boundary (not remain due this minute). */
	ASSERT(next >= (now - (now % 60) + 60));
	time_t next_t = (time_t)next;
	struct tm next_tm;
	ASSERT(localtime_r(&next_t, &next_tm) != NULL);
	ASSERT(next_tm.tm_min == tm.tm_min);
	ASSERT(next_tm.tm_hour == tm.tm_hour);
	return 0;
}

static int test_cron_expr_monthly_beyond_eight_days(void)
{
	/* Anchor mid-month so the 1st of next month is > 8 days away. */
	time_t t = 1700000000; /* 2023-11-14 local-ish depending on TZ */
	struct tm tm;
	ASSERT(localtime_r(&t, &tm) != NULL);
	tm.tm_mday = 15;
	tm.tm_hour = 12;
	tm.tm_min = 0;
	tm.tm_sec = 0;
	tm.tm_isdst = -1;
	t = mktime(&tm);
	ASSERT(t != (time_t)-1);
	long long now = (long long)t;
	long long next = 0;
	ASSERT(cron_parse_next_run("cron:0 0 1 * *", now, &next) == 0);
	ASSERT(next > now);
	ASSERT(next - now > 8LL * 24 * 3600);
	time_t next_t = (time_t)next;
	struct tm next_tm;
	ASSERT(localtime_r(&next_t, &next_tm) != NULL);
	ASSERT(next_tm.tm_mday == 1);
	ASSERT(next_tm.tm_hour == 0);
	ASSERT(next_tm.tm_min == 0);
	return 0;
}

static int test_cron_poll_advances_recurring_past_due_minute(void)
{
	const char *path = "/tmp/shellclaw_test_cron_poll_advance.db";
	remove(path);
	ASSERT(memory_init(path) == 0);

	long long now = (long long)time(NULL);
	time_t t = (time_t)now;
	struct tm tm;
	ASSERT(localtime_r(&t, &tm) != NULL);
	char schedule[64];
	snprintf(schedule, sizeof(schedule), "cron:%d %d * * *", tm.tm_min, tm.tm_hour);

	ASSERT(cron_job_create("poll_adv1", schedule, "due now", "cli", "default", now - 1, 1) == 0);

	const channel_t *cron_ch = channel_cron_get();
	ASSERT(cron_ch != NULL && cron_ch->poll != NULL);
	channel_incoming_msg_t msg;
	memset(&msg, 0, sizeof(msg));
	ASSERT(cron_ch->poll(&msg, 0) == 1);
	ASSERT(msg.text != NULL);
	free(msg.session_id);
	free(msg.user_id);
	free(msg.text);

	cron_job_row_t rows[4];
	ASSERT(cron_job_list(rows, 4) == 1);
	ASSERT(strcmp(rows[0].id, "poll_adv1") == 0);
	ASSERT(rows[0].next_run > now);
	ASSERT(rows[0].next_run >= (now - (now % 60) + 60));

	memset(&msg, 0, sizeof(msg));
	ASSERT(cron_ch->poll(&msg, 0) == 0);

	cron_job_delete("poll_adv1");
	memory_cleanup();
	remove(path);
	return 0;
}

static int test_invalid_schedule(void)
{
	long long now = 1700000000;
	long long next = 0;
	ASSERT(cron_parse_next_run("invalid", now, &next) == -1);
	ASSERT(cron_parse_next_run("interval:0", now, &next) == -1);
	ASSERT(cron_parse_next_run("interval:-1", now, &next) == -1);
	return 0;
}

static int test_cron_job_crud_and_due(void)
{
	const char *path = "/tmp/shellclaw_test_cron.db";
	remove(path);
	ASSERT(memory_init(path) == 0);
	long long now = (long long)time(NULL);
	long long next = 0;
	ASSERT(cron_parse_next_run("at:9999999999", now, &next) == 0);
	ASSERT(cron_job_create("job1", "at:9999999999", "Remind me", "cli", "default", next, 1) == 0);
	cron_job_row_t rows[16];
	int n = cron_job_list(rows, 16);
	ASSERT(n == 1);
	ASSERT(strcmp(rows[0].id, "job1") == 0);
	ASSERT(strcmp(rows[0].message, "Remind me") == 0);
	cron_job_row_t due;
	ASSERT(cron_job_get_next_due(now, &due) == 0);
	ASSERT(cron_job_get_next_due(9999999999, &due) == 1);
	ASSERT(strcmp(due.id, "job1") == 0);
	ASSERT(cron_job_toggle("job1") == 0);
	ASSERT(cron_job_get_next_due(9999999999, &due) == 0);
	ASSERT(cron_job_toggle("job1") == 0);
	ASSERT(cron_job_delete("job1") == 0);
	ASSERT(cron_job_list(rows, 16) == 0);
	memory_cleanup();
	remove(path);
	return 0;
}

static int test_cron_tool_execute(void)
{
	const char *path = "/tmp/shellclaw_test_cron_tool.db";
	remove(path);
	ASSERT(memory_init(path) == 0);
	const tool_t *cron_tool = tool_cron_get();
	ASSERT(cron_tool != NULL);
	char buf[4096];
	ASSERT(cron_tool->execute("{\"operation\":\"list\"}", buf, sizeof(buf)) == 0);
	ASSERT(strstr(buf, "[") != NULL);
	ASSERT(cron_tool->execute("{\"operation\":\"create\",\"schedule\":\"interval:60\",\"message\":\"test\"}", buf, sizeof(buf)) == 0);
	ASSERT(strstr(buf, "\"ok\":true") != NULL);
	ASSERT(strstr(buf, "\"id\"") != NULL);
	ASSERT(cron_tool->execute("{\"operation\":\"list\"}", buf, sizeof(buf)) == 0);
	ASSERT(strstr(buf, "test") != NULL);
	cron_job_row_t rows[16];
	int n = cron_job_list(rows, 16);
	ASSERT(n >= 1);
	const char *id = rows[0].id;
	char del_json[256];
	snprintf(del_json, sizeof(del_json), "{\"operation\":\"delete\",\"id\":\"%s\"}", id);
	ASSERT(cron_tool->execute(del_json, buf, sizeof(buf)) == 0);
	ASSERT(cron_tool->execute("{\"operation\":\"list\"}", buf, sizeof(buf)) == 0);
	ASSERT(strstr(buf, "test") == NULL);
	memory_cleanup();
	remove(path);
	return 0;
}

static int test_one_shot_detection(void)
{
	ASSERT(cron_is_one_shot("at:9999999999") == 1);
	ASSERT(cron_is_one_shot("interval:3600") == 0);
	ASSERT(cron_is_one_shot("0 9 * * 1-5") == 0);
	const char *path = "/tmp/shellclaw_test_cron_oneshot.db";
	remove(path);
	ASSERT(memory_init(path) == 0);
	ASSERT(cron_job_create("oneshot1", "at:9999999999", "One-shot", "cli", "default", 9999999999, 1) == 0);
	cron_job_row_t due;
	ASSERT(cron_job_get_next_due(9999999999, &due) == 1);
	ASSERT(cron_is_one_shot(due.schedule) == 1);
	cron_job_delete("oneshot1");
	memory_cleanup();
	remove(path);
	return 0;
}

int main(void)
{
	RUN(test_interval_next_run());
	RUN(test_at_one_shot());
	RUN(test_cron_expr_next_run());
	RUN(test_cron_expr_with_prefix());
	RUN(test_cron_expr_skips_current_minute());
	RUN(test_cron_expr_monthly_beyond_eight_days());
	RUN(test_cron_poll_advances_recurring_past_due_minute());
	RUN(test_invalid_schedule());
	RUN(test_cron_job_crud_and_due());
	RUN(test_cron_tool_execute());
	RUN(test_one_shot_detection());
	printf("test_cron: all tests passed\n");
	return 0;
}
