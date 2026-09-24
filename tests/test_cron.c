/**
 * @file test_cron.c
 * @brief Unit tests for cron: schedule parsing, next_run, one-shot.
 */
#define _POSIX_C_SOURCE 200809L

#include "tools/cron.h"
#include "core/memory.h"
#include "channels/channel.h"
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
	long long now = 1700000017;
	time_t t = (time_t)now;
	struct tm tm;
	char schedule[64];
	long long next = 0;
	time_t next_t;
	struct tm next_tm;

	ASSERT(localtime_r(&t, &tm) != NULL);
	snprintf(schedule, sizeof(schedule), "cron:%d %d * * *", tm.tm_min, tm.tm_hour);
	ASSERT(cron_parse_next_run(schedule, now, &next) == 0);
	ASSERT(next > now);
	ASSERT(next >= (now - (now % 60) + 60));
	next_t = (time_t)next;
	ASSERT(localtime_r(&next_t, &next_tm) != NULL);
	ASSERT(next_tm.tm_min == tm.tm_min);
	ASSERT(next_tm.tm_hour == tm.tm_hour);
	return 0;
}

static int test_cron_expr_monthly_beyond_eight_days(void)
{
	time_t t = 1700000000;
	struct tm tm;
	long long now;
	long long next = 0;
	time_t next_t;
	struct tm next_tm;

	ASSERT(localtime_r(&t, &tm) != NULL);
	tm.tm_mday = 15;
	tm.tm_hour = 12;
	tm.tm_min = 0;
	tm.tm_sec = 0;
	tm.tm_isdst = -1;
	t = mktime(&tm);
	ASSERT(t != (time_t)-1);
	now = (long long)t;
	ASSERT(cron_parse_next_run("cron:0 0 1 * *", now, &next) == 0);
	ASSERT(next > now);
	ASSERT(next - now > 8LL * 24 * 3600);
	next_t = (time_t)next;
	ASSERT(localtime_r(&next_t, &next_tm) != NULL);
	ASSERT(next_tm.tm_mday == 1);
	ASSERT(next_tm.tm_hour == 0);
	ASSERT(next_tm.tm_min == 0);
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
	memset(rows, 0, sizeof(rows));
	int n = cron_job_list(rows, 16);
	ASSERT(n == 1);
	ASSERT(strcmp(rows[0].id, "job1") == 0);
	ASSERT(strcmp(rows[0].message, "Remind me") == 0);
	for (int i = 0; i < n; i++) cron_job_row_free(&rows[i]);
	cron_job_row_t due;
	memset(&due, 0, sizeof(due));
	ASSERT(cron_job_get_next_due(now, &due) == 0);
	ASSERT(cron_job_get_next_due(9999999999, &due) == 1);
	ASSERT(strcmp(due.id, "job1") == 0);
	cron_job_row_free(&due);
	ASSERT(cron_job_toggle("job1") == 0);
	memset(&due, 0, sizeof(due));
	ASSERT(cron_job_get_next_due(9999999999, &due) == 0);
	cron_job_row_free(&due);
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
	memset(rows, 0, sizeof(rows));
	int n = cron_job_list(rows, 16);
	ASSERT(n >= 1);
	char id[128];
	snprintf(id, sizeof(id), "%s", rows[0].id);
	for (int i = 0; i < n; i++) cron_job_row_free(&rows[i]);
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
	memset(&due, 0, sizeof(due));
	ASSERT(cron_job_get_next_due(9999999999, &due) == 1);
	ASSERT(cron_is_one_shot(due.schedule) == 1);
	cron_job_row_free(&due);
	cron_job_delete("oneshot1");
	memory_cleanup();
	remove(path);
	return 0;
}

static int test_due_job_delivers_full_message(void)
{
	const char *path = "/tmp/shellclaw_test_cron_long_message.db";
	remove(path);
	ASSERT(memory_init(path) == 0);
	char message[600];
	memset(message, 'A', 599);
	message[599] = '\0';
	long long now = (long long)time(NULL);
	ASSERT(cron_job_create("longmsg", "interval:60", message, "cli", "default", now - 1, 1) == 0);
	cron_job_row_t due;
	memset(&due, 0, sizeof(due));
	ASSERT(cron_job_get_next_due(now, &due) == 1);
	ASSERT(due.message != NULL);
	ASSERT(strlen(due.message) == 599);
	ASSERT(strcmp(due.message, message) == 0);
	cron_job_row_free(&due);
	const channel_t *ch = channel_cron_get();
	channel_incoming_msg_t msg;
	memset(&msg, 0, sizeof(msg));
	ASSERT(ch->poll(&msg, 0) == 1);
	ASSERT(msg.text != NULL);
	ASSERT(strlen(msg.text) == 599);
	ASSERT(strcmp(msg.text, message) == 0);
	channel_incoming_msg_clear(&msg);
	memory_cleanup();
	remove(path);
	return 0;
}

static int test_long_interval_schedule_roundtrips(void)
{
	const char *path = "/tmp/shellclaw_test_cron_long_schedule.db";
	const channel_t *ch;
	channel_incoming_msg_t msg;
	cron_job_row_t due;
	cron_job_row_t after;
	char zeros[121];
	char schedule[160];
	long long parsed = 0;
	long long now;
	remove(path);
	ASSERT(memory_init(path) == 0);
	memset(zeros, '0', 120);
	zeros[120] = '\0';
	snprintf(schedule, sizeof(schedule), "interval:%s60", zeros);
	ASSERT(strlen(schedule) > 127);
	now = (long long)time(NULL);
	ASSERT(cron_parse_next_run(schedule, now, &parsed) == 0);
	ASSERT(parsed == now + 60);
	ASSERT(cron_job_create("longsched", schedule, "tick", "cli", "default", now - 1, 1) == 0);
	memset(&due, 0, sizeof(due));
	ASSERT(cron_job_get_next_due(now, &due) == 1);
	ASSERT(due.schedule != NULL);
	ASSERT(strcmp(due.schedule, schedule) == 0);
	cron_job_row_free(&due);
	ch = channel_cron_get();
	memset(&msg, 0, sizeof(msg));
	ASSERT(ch->poll(&msg, 0) == 1);
	ASSERT(msg.user_id != NULL);
	ASSERT(strcmp(msg.user_id, "longsched") == 0);
	ASSERT(cron_ack_delivery(msg.user_id) == 0);
	channel_incoming_msg_clear(&msg);
	memset(&after, 0, sizeof(after));
	ASSERT(cron_job_get_by_id("longsched", &after) == 1);
	ASSERT(after.next_run > now);
	ASSERT(after.schedule != NULL);
	ASSERT(strcmp(after.schedule, schedule) == 0);
	cron_job_row_free(&after);
	memset(&msg, 0, sizeof(msg));
	ASSERT(ch->poll(&msg, 0) == 0);
	memory_cleanup();
	remove(path);
	return 0;
}

static int test_cron_ack_delivery_deferred(void)
{
	const char *path = "/tmp/shellclaw_test_cron_ack.db";
	remove(path);
	ASSERT(memory_init(path) == 0);
	long long now = (long long)time(NULL);
	long long due_at = now - 10;
	ASSERT(cron_job_create("oneshot_ack", "at:9999999999", "Fire me", "cli", "default", due_at, 1) == 0);
	cron_job_row_t row;
	memset(&row, 0, sizeof(row));
	ASSERT(cron_job_get_next_due(now, &row) == 1);
	ASSERT(strcmp(row.id, "oneshot_ack") == 0);
	cron_job_row_free(&row);
	memset(&row, 0, sizeof(row));
	ASSERT(cron_job_get_by_id("oneshot_ack", &row) == 1);
	cron_job_row_free(&row);
	ASSERT(cron_ack_delivery("oneshot_ack") == 0);
	ASSERT(cron_job_get_by_id("oneshot_ack", &row) == 0);
	ASSERT(cron_job_create("interval_ack", "interval:3600", "Repeat", "cli", "default", due_at, 1) == 0);
	memset(&row, 0, sizeof(row));
	ASSERT(cron_job_get_next_due(now, &row) == 1);
	long long before_ack = row.next_run;
	cron_job_row_free(&row);
	ASSERT(cron_ack_delivery("interval_ack") == 0);
	memset(&row, 0, sizeof(row));
	ASSERT(cron_job_get_by_id("interval_ack", &row) == 1);
	ASSERT(row.next_run > before_ack);
	cron_job_row_free(&row);
	memory_cleanup();
	remove(path);
	return 0;
}

static int test_cron_poll_keeps_job_until_ack(void)
{
	const channel_t *cron_ch = channel_cron_get();
	ASSERT(cron_ch != NULL);
	const char *path = "/tmp/shellclaw_test_cron_poll.db";
	remove(path);
	ASSERT(memory_init(path) == 0);
	long long now = (long long)time(NULL);
	ASSERT(cron_job_create("poll_keep", "at:9999999999", "Due now", "cli", "default", now - 1, 1) == 0);
	channel_incoming_msg_t msg;
	memset(&msg, 0, sizeof(msg));
	ASSERT(cron_ch->poll(&msg, 0) == 1);
	ASSERT(msg.user_id != NULL);
	ASSERT(strcmp(msg.user_id, "poll_keep") == 0);
	cron_job_row_t row;
	memset(&row, 0, sizeof(row));
	ASSERT(cron_job_get_by_id("poll_keep", &row) == 1);
	cron_job_row_free(&row);
	channel_incoming_msg_clear(&msg);
	ASSERT(cron_ack_delivery("poll_keep") == 0);
	ASSERT(cron_job_get_by_id("poll_keep", &row) == 0);
	memory_cleanup();
	remove(path);
	return 0;
}

static int test_cron_ack_advances_recurring_past_due_minute(void)
{
	const char *path = "/tmp/shellclaw_test_cron_ack_advance.db";
	const channel_t *cron_ch;
	channel_incoming_msg_t msg;
	cron_job_row_t rows[4];
	long long now;
	time_t t;
	struct tm tm;
	char schedule[64];

	remove(path);
	ASSERT(memory_init(path) == 0);
	now = (long long)time(NULL);
	t = (time_t)now;
	ASSERT(localtime_r(&t, &tm) != NULL);
	snprintf(schedule, sizeof(schedule), "cron:%d %d * * *", tm.tm_min, tm.tm_hour);
	ASSERT(cron_job_create("poll_adv1", schedule, "due now", "cli", "default", now - 1, 1) == 0);
	cron_ch = channel_cron_get();
	ASSERT(cron_ch != NULL && cron_ch->poll != NULL);
	memset(&msg, 0, sizeof(msg));
	ASSERT(cron_ch->poll(&msg, 0) == 1);
	ASSERT(msg.user_id != NULL);
	ASSERT(cron_ack_delivery(msg.user_id) == 0);
	channel_incoming_msg_clear(&msg);
	memset(rows, 0, sizeof(rows));
	ASSERT(cron_job_list(rows, 4) == 1);
	ASSERT(strcmp(rows[0].id, "poll_adv1") == 0);
	ASSERT(rows[0].next_run > now);
	ASSERT(rows[0].next_run >= (now - (now % 60) + 60));
	cron_job_row_free(&rows[0]);
	memset(&msg, 0, sizeof(msg));
	ASSERT(cron_ch->poll(&msg, 0) == 0);
	cron_job_delete("poll_adv1");
	memory_cleanup();
	remove(path);
	return 0;
}

static int test_cron_ack_fail_closed_on_parse_error(void)
{
	const char *path = "/tmp/shellclaw_test_cron_ack_fail_closed.db";
	cron_job_row_t row;
	long long now;
	long long floor_next;
	long long ceil_next;

	remove(path);
	ASSERT(memory_init(path) == 0);
	now = (long long)time(NULL);
	ASSERT(cron_job_create("bad_sched", "cron:not-a-schedule", "msg", "cli", "default", now - 1, 1) == 0);
	ASSERT(cron_ack_delivery("bad_sched") == 0);
	memset(&row, 0, sizeof(row));
	ASSERT(cron_job_get_by_id("bad_sched", &row) == 1);
	floor_next = now + 365LL * 24 * 3600 - 2;
	ceil_next = now + 365LL * 24 * 3600 + 2;
	ASSERT(row.next_run >= floor_next);
	ASSERT(row.next_run <= ceil_next);
	cron_job_row_free(&row);
	memory_cleanup();
	remove(path);
	return 0;
}

static int test_cron_poll_waits_before_reoffer(void)
{
	const char *path = "/tmp/shellclaw_test_cron_reoffer.db";
	const channel_t *cron_ch;
	channel_incoming_msg_t msg;
	struct timespec t0;
	struct timespec t1;
	long elapsed_ms;
	long long now;
	remove(path);
	ASSERT(memory_init(path) == 0);
	now = (long long)time(NULL);
	ASSERT(cron_job_create("reoffer", "interval:60", "again", "cli", "default", now - 1, 1) == 0);
	cron_ch = channel_cron_get();
	memset(&msg, 0, sizeof(msg));
	ASSERT(cron_ch->poll(&msg, 0) == 1);
	channel_incoming_msg_clear(&msg);
	memset(&msg, 0, sizeof(msg));
	ASSERT(clock_gettime(CLOCK_MONOTONIC, &t0) == 0);
	ASSERT(cron_ch->poll(&msg, 80) == 1);
	ASSERT(clock_gettime(CLOCK_MONOTONIC, &t1) == 0);
	elapsed_ms = (t1.tv_sec - t0.tv_sec) * 1000L + (t1.tv_nsec - t0.tv_nsec) / 1000000L;
	ASSERT(elapsed_ms >= 40);
	channel_incoming_msg_clear(&msg);
	memory_cleanup();
	remove(path);
	return 0;
}

static int test_cron_job_rejects_oversized_text(void)
{
	const char *path = "/tmp/shellclaw_test_cron_text_cap.db";
	char *too_big;
	remove(path);
	ASSERT(memory_init(path) == 0);
	too_big = malloc((size_t)CRON_JOB_TEXT_MAX + 2);
	ASSERT(too_big != NULL);
	memset(too_big, 'A', (size_t)CRON_JOB_TEXT_MAX + 1);
	too_big[CRON_JOB_TEXT_MAX + 1] = '\0';
	ASSERT(cron_job_create("bigmsg", "interval:60", too_big, "cli", "default", 1, 1) != 0);
	ASSERT(cron_job_create("bigsched", too_big, "tick", "cli", "default", 1, 1) != 0);
	free(too_big);
	memory_cleanup();
	remove(path);
	return 0;
}

/* An unacked earlier job must not hide another job that is also due. */
static int test_cron_poll_offers_sibling_while_earlier_unacked(void)
{
	const char *path = "/tmp/shellclaw_test_cron_sibling.db";
	const channel_t *cron_ch;
	channel_incoming_msg_t msg;
	cron_job_row_t row;
	struct timespec t0;
	struct timespec t1;
	long elapsed_ms;
	long long now;

	remove(path);
	ASSERT(memory_init(path) == 0);
	now = (long long)time(NULL);
	ASSERT(cron_job_create("job_a", "interval:60", "A", "cli", "default", now - 10, 1) == 0);
	ASSERT(cron_job_create("job_b", "interval:60", "B", "cli", "default", now - 5, 1) == 0);
	cron_ch = channel_cron_get();
	ASSERT(cron_ch != NULL && cron_ch->poll != NULL);
	memset(&msg, 0, sizeof(msg));
	ASSERT(cron_ch->poll(&msg, 0) == 1);
	ASSERT(msg.user_id != NULL);
	ASSERT(strcmp(msg.user_id, "job_a") == 0);
	channel_incoming_msg_clear(&msg);
	memset(&msg, 0, sizeof(msg));
	ASSERT(clock_gettime(CLOCK_MONOTONIC, &t0) == 0);
	ASSERT(cron_ch->poll(&msg, 2000) == 1);
	ASSERT(clock_gettime(CLOCK_MONOTONIC, &t1) == 0);
	elapsed_ms = (t1.tv_sec - t0.tv_sec) * 1000L
		+ (t1.tv_nsec - t0.tv_nsec) / 1000000L;
	ASSERT(msg.user_id != NULL);
	ASSERT(strcmp(msg.user_id, "job_b") == 0);
	ASSERT(elapsed_ms < 400);
	channel_incoming_msg_clear(&msg);
	memset(&row, 0, sizeof(row));
	ASSERT(cron_job_get_by_id("job_a", &row) == 1);
	ASSERT(row.next_run == now - 10);
	cron_job_row_free(&row);
	memset(&row, 0, sizeof(row));
	ASSERT(cron_job_get_by_id("job_b", &row) == 1);
	ASSERT(row.next_run == now - 5);
	cron_job_row_free(&row);
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
	RUN(test_invalid_schedule());
	RUN(test_cron_job_crud_and_due());
	RUN(test_cron_tool_execute());
	RUN(test_one_shot_detection());
	RUN(test_due_job_delivers_full_message());
	RUN(test_long_interval_schedule_roundtrips());
	RUN(test_cron_ack_delivery_deferred());
	RUN(test_cron_poll_keeps_job_until_ack());
	RUN(test_cron_ack_advances_recurring_past_due_minute());
	RUN(test_cron_ack_fail_closed_on_parse_error());
	RUN(test_cron_poll_waits_before_reoffer());
	RUN(test_cron_job_rejects_oversized_text());
	RUN(test_cron_poll_offers_sibling_while_earlier_unacked());
	printf("test_cron: all tests passed\n");
	return 0;
}
