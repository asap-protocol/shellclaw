/**
 * @file test_dispatch.c
 * @brief Unit tests for handle_message slash commands and agent dispatch.
 */
#define _POSIX_C_SOURCE 200809L

#include "channels/channel.h"
#include "core/bootstrap.h"

void bootstrap_set_provider_for_test(const provider_t *provider);
void bootstrap_reset_tools_for_test(void);
void bootstrap_add_tool_for_test(const tool_t *tool);
#include "core/config.h"
#include "core/dispatch.h"
#include "core/memory.h"
#include "providers/provider.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define ASSERT(c)                                                                              \
	do {                                                                                   \
		if (!(c)) {                                                                    \
			fprintf(stderr, "FAIL: %s:%d %s\n", __FILE__, __LINE__, #c);          \
			return 1;                                                              \
		}                                                                              \
	} while (0)
#define RUN(t)                                                                                 \
	do {                                                                                   \
		int r = (t);                                                                   \
		if (r)                                                                         \
			return r;                                                              \
	} while (0)

#define SEND_BUF_SIZE 4096
#define DISPATCH_FULL_TOOL_COUNT 13
static char g_last_session[SEND_BUF_SIZE];
static char g_last_text[SEND_BUF_SIZE];
static int g_send_calls;
static size_t g_last_provider_tool_count;

static int mock_send(const char *session_id, const char *text,
		     const channel_attachment_t *attachments, size_t attachments_count)
{
	(void)attachments;
	(void)attachments_count;
	g_send_calls++;
	strncpy(g_last_session, session_id ? session_id : "", sizeof(g_last_session) - 1);
	g_last_session[sizeof(g_last_session) - 1] = '\0';
	strncpy(g_last_text, text ? text : "", sizeof(g_last_text) - 1);
	g_last_text[sizeof(g_last_text) - 1] = '\0';
	return 0;
}

static const channel_t mock_channel = {
	.name = "mock",
	.init = NULL,
	.poll = NULL,
	.send = mock_send,
	.cleanup = NULL,
};

static int spy_init(const config_t *cfg)
{
	(void)cfg;
	return 0;
}

static int spy_chat(const provider_message_t *messages, size_t message_count,
		    const provider_tool_def_t *tools, size_t tool_count,
		    provider_response_t *response)
{
	(void)messages;
	(void)message_count;
	(void)tools;
	g_last_provider_tool_count = tool_count;
	response->error = 0;
	response->content = strdup("agent-ok");
	response->tool_calls = NULL;
	response->tool_calls_count = 0;
	return 0;
}

static void spy_cleanup(void) {}

static const provider_t spy_provider = {
	.name = "spy",
	.init = spy_init,
	.chat = spy_chat,
	.cleanup = spy_cleanup,
};

static int fail_chat(const provider_message_t *messages, size_t message_count,
		     const provider_tool_def_t *tools, size_t tool_count,
		     provider_response_t *response)
{
	(void)messages;
	(void)message_count;
	(void)tools;
	(void)tool_count;
	response->error = 1;
	response->content = NULL;
	response->tool_calls = NULL;
	response->tool_calls_count = 0;
	return -3;
}

static const provider_t fail_provider = {
	.name = "fail",
	.init = spy_init,
	.chat = fail_chat,
	.cleanup = spy_cleanup,
};

static config_t *load_minimal_cfg(const char *path)
{
	config_t *cfg = NULL;
	char errbuf[256];

	if (config_load(path, &cfg, errbuf, sizeof(errbuf)) != 0)
		return NULL;
	return cfg;
}

static int write_minimal_toml(const char *path)
{
	FILE *fp = fopen(path, "w");

	if (!fp)
		return -1;
	fprintf(fp,
		"[channels.discord]\n"
		"enabled = false\n"
		"[agent]\n"
		"model = \"stub\"\n"
		"[providers]\n"
		"fallback_chain = [\"stub\"]\n");
	fclose(fp);
	return 0;
}

static void reset_send_spy(void)
{
	g_send_calls = 0;
	g_last_session[0] = '\0';
	g_last_text[0] = '\0';
	g_last_provider_tool_count = 0;
}

static int test_reset_clears_session(void)
{
	const char *db_path = "build/test_dispatch_reset.db";
	char tmpl[] = "/tmp/shellclaw_test_dispatch_XXXXXX";
	channel_incoming_msg_t msg = {0};
	config_t *cfg = NULL;
	char history[512];
	int fd;

	reset_send_spy();
	memory_cleanup();
	ASSERT(memory_init(db_path) == 0);
	ASSERT(session_save("ws:test", "[{\"role\":\"user\",\"content\":\"hi\"}]") == 0);

	fd = mkstemp(tmpl);
	ASSERT(fd >= 0);
	close(fd);
	ASSERT(write_minimal_toml(tmpl) == 0);
	cfg = load_minimal_cfg(tmpl);
	ASSERT(cfg != NULL);
	bootstrap_set_cfg(cfg);
	bootstrap_reset_tools_for_test();

	msg.session_id = "ws:test";
	msg.text = "/reset";
	ASSERT(handle_message(&mock_channel, &msg) == 0);
	ASSERT(g_send_calls == 1);
	ASSERT(strstr(g_last_text, "Session cleared") != NULL);
	ASSERT(session_load("ws:test", history, sizeof(history)) != 0);

	config_free(cfg);
	unlink(tmpl);
	memory_cleanup();
	return 0;
}

static int test_status_returns_version(void)
{
	channel_incoming_msg_t msg = {0};

	reset_send_spy();
	msg.session_id = "cli:test";
	msg.text = "/status";
	ASSERT(handle_message(&mock_channel, &msg) == 0);
	ASSERT(g_send_calls == 1);
	ASSERT(strstr(g_last_text, "ShellClaw 0.2.0") != NULL);
	ASSERT(strstr(g_last_text, "agent ready") != NULL);
	return 0;
}

static int test_agent_failure_fallback_message(void)
{
	channel_incoming_msg_t msg = {0};
	char tmpl[] = "/tmp/shellclaw_test_dispatch_fail_XXXXXX";
	config_t *cfg = NULL;
	int fd;

	reset_send_spy();
	fd = mkstemp(tmpl);
	ASSERT(fd >= 0);
	close(fd);
	ASSERT(write_minimal_toml(tmpl) == 0);
	cfg = load_minimal_cfg(tmpl);
	ASSERT(cfg != NULL);
	bootstrap_set_cfg(cfg);
	bootstrap_set_provider_for_test(&fail_provider);
	bootstrap_reset_tools_for_test();

	msg.session_id = "cli:fail";
	msg.text = "hello";
	ASSERT(handle_message(&mock_channel, &msg) == 0);
	ASSERT(g_send_calls == 1);
	ASSERT(strstr(g_last_text, "Error: agent failed (code -1)") != NULL);

	config_free(cfg);
	unlink(tmpl);
	return 0;
}

static int test_normal_message_uses_provider(void)
{
	channel_incoming_msg_t msg = {0};
	char tmpl[] = "/tmp/shellclaw_test_dispatch_spy_XXXXXX";
	config_t *cfg = NULL;
	int fd;

	reset_send_spy();
	fd = mkstemp(tmpl);
	ASSERT(fd >= 0);
	close(fd);
	ASSERT(write_minimal_toml(tmpl) == 0);
	cfg = load_minimal_cfg(tmpl);
	ASSERT(cfg != NULL);
	bootstrap_set_cfg(cfg);
	bootstrap_set_provider_for_test(&spy_provider);
	bootstrap_reset_tools_for_test();

	msg.session_id = "cli:spy";
	msg.text = "ping";
	ASSERT(handle_message(&mock_channel, &msg) == 0);
	ASSERT(g_send_calls == 1);
	ASSERT(strstr(g_last_text, "agent-ok") != NULL);

	config_free(cfg);
	unlink(tmpl);
	return 0;
}

static int dummy_tool_exec(const char *args_json, char *result_buf, size_t max_len)
{
	(void)args_json;
	if (result_buf && max_len > 0)
		result_buf[0] = '\0';
	return 0;
}

static int test_dispatch_forwards_full_hardware_tool_table(void)
{
	channel_incoming_msg_t msg = {0};
	char tmpl[] = "/tmp/shellclaw_test_dispatch_tools_XXXXXX";
	config_t *cfg = NULL;
	static tool_t tools[DISPATCH_FULL_TOOL_COUNT];
	static char names[DISPATCH_FULL_TOOL_COUNT][8];
	size_t i;
	int fd;

	reset_send_spy();
	g_last_provider_tool_count = 0;
	fd = mkstemp(tmpl);
	ASSERT(fd >= 0);
	close(fd);
	ASSERT(write_minimal_toml(tmpl) == 0);
	cfg = load_minimal_cfg(tmpl);
	ASSERT(cfg != NULL);
	bootstrap_set_cfg(cfg);
	bootstrap_set_provider_for_test(&spy_provider);
	bootstrap_reset_tools_for_test();
	for (i = 0; i < DISPATCH_FULL_TOOL_COUNT; i++) {
		snprintf(names[i], sizeof(names[i]), "t%zu", i);
		tools[i].name = names[i];
		tools[i].description = "d";
		tools[i].parameters_json = "{}";
		tools[i].execute = dummy_tool_exec;
		bootstrap_add_tool_for_test(&tools[i]);
	}
	msg.session_id = "cli:tools";
	msg.text = "ping";
	ASSERT(handle_message(&mock_channel, &msg) == 0);
	ASSERT(g_last_provider_tool_count == DISPATCH_FULL_TOOL_COUNT);
	config_free(cfg);
	unlink(tmpl);
	return 0;
}

int main(void)
{
	RUN(test_reset_clears_session());
	RUN(test_status_returns_version());
	RUN(test_agent_failure_fallback_message());
	RUN(test_normal_message_uses_provider());
	RUN(test_dispatch_forwards_full_hardware_tool_table());
	puts("test_dispatch OK");
	return 0;
}
