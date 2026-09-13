/**
 * @file test_agent.c
 * @brief Unit tests for agent_run: API, stub, context assembly, ReAct loop (Tasks 5.1, 5.2, 5.3).
 */
#define _POSIX_C_SOURCE 200809L

#include "core/agent.h"
#include "core/config.h"
#include "core/memory.h"
#include "providers/provider.h"
#include "cJSON.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define ASSERT(c) do { if (!(c)) { fprintf(stderr, "FAIL: %s:%d %s\n", __FILE__, __LINE__, #c); return 1; } } while (0)
#define RUN(t) do { int r = (t); if (r) return r; } while (0)

#define SPY_CONTENT_SIZE 4096
#define SPY_SLOTS 8
static size_t spy_message_count;
static size_t spy_first_content_len;
static char spy_first_last_char;
static char spy_content[SPY_SLOTS][SPY_CONTENT_SIZE];
static char *spy_roles[SPY_SLOTS];

static int spy_init(const config_t *cfg) { (void)cfg; return 0; }
static void spy_roles_clear(void)
{
	for (size_t i = 0; i < SPY_SLOTS; i++) {
		free(spy_roles[i]);
		spy_roles[i] = NULL;
	}
}
static int spy_chat(const provider_message_t *messages, size_t message_count,
	const provider_tool_def_t *tools, size_t tool_count, provider_response_t *response)
{
	(void)tools;
	(void)tool_count;
	response->error = 0;
	response->content = strdup("ok");
	response->tool_calls = NULL;
	response->tool_calls_count = 0;
	spy_roles_clear();
	spy_message_count = message_count;
	spy_first_content_len = (message_count > 0 && messages[0].content)
		? strlen(messages[0].content) : 0;
	spy_first_last_char = (spy_first_content_len > 0 && messages[0].content)
		? messages[0].content[spy_first_content_len - 1] : '\0';
	for (size_t i = 0; i < message_count && i < SPY_SLOTS; i++) {
		spy_roles[i] = messages[i].role ? strdup(messages[i].role) : NULL;
		if (messages[i].content) {
			size_t n = strlen(messages[i].content);
			if (n >= SPY_CONTENT_SIZE) n = SPY_CONTENT_SIZE - 1;
			memcpy(spy_content[i], messages[i].content, n);
			spy_content[i][n] = '\0';
		} else
			spy_content[i][0] = '\0';
	}
	return 0;
}
static void spy_cleanup(void) { spy_roles_clear(); }
static const provider_t spy_provider = {
	.name = "spy",
	.init = spy_init,
	.chat = spy_chat,
	.cleanup = spy_cleanup,
};

static int tool_then_text_call_count;
static int tool_then_text_init(const config_t *cfg) { (void)cfg; tool_then_text_call_count = 0; return 0; }
static int tool_then_text_chat(const provider_message_t *messages, size_t message_count,
	const provider_tool_def_t *tools, size_t tool_count, provider_response_t *response)
{
	(void)messages;
	(void)message_count;
	(void)tools;
	(void)tool_count;
	response->error = 0;
	response->tool_calls = NULL;
	response->tool_calls_count = 0;
	response->content = NULL;
	tool_then_text_call_count++;
	if (tool_then_text_call_count == 1) {
		response->tool_calls = malloc(sizeof(provider_tool_call_t));
		if (response->tool_calls) {
			response->tool_calls[0].id = strdup("tc1");
			response->tool_calls[0].name = strdup("echo");
			response->tool_calls[0].arguments = strdup("{}");
			response->tool_calls_count = 1;
		}
		response->content = strdup("");
	} else {
		response->content = strdup("final text");
	}
	return 0;
}
static void tool_then_text_cleanup(void) {}
static const provider_t tool_then_text_provider = {
	.name = "tool_then_text",
	.init = tool_then_text_init,
	.chat = tool_then_text_chat,
	.cleanup = tool_then_text_cleanup,
};

static int always_tool_call_count;
static int always_tool_init(const config_t *cfg) { (void)cfg; always_tool_call_count = 0; return 0; }
static int always_tool_chat(const provider_message_t *messages, size_t message_count,
	const provider_tool_def_t *tools, size_t tool_count, provider_response_t *response)
{
	(void)messages;
	(void)message_count;
	(void)tools;
	(void)tool_count;
	response->error = 0;
	always_tool_call_count++;
	response->content = strdup("partial");
	response->tool_calls = malloc(sizeof(provider_tool_call_t));
	if (!response->tool_calls) {
		free(response->content);
		response->content = NULL;
		response->error = 1;
		return -1;
	}
	response->tool_calls[0].id = strdup("tid");
	response->tool_calls[0].name = strdup("echo");
	response->tool_calls[0].arguments = strdup("{}");
	response->tool_calls_count = 1;
	return 0;
}
static void always_tool_cleanup(void) {}
static const provider_t always_tool_provider = {
	.name = "always_tool",
	.init = always_tool_init,
	.chat = always_tool_chat,
	.cleanup = always_tool_cleanup,
};

static int mock_echo_execute(const char *args_json, char *result_buf, size_t max_len)
{
	(void)args_json;
	if (max_len > 0) {
		strncpy(result_buf, "echo result", max_len - 1);
		result_buf[max_len - 1] = '\0';
	}
	return 0;
}
static const agent_tool_t mock_echo_tool = {
	.name = "echo",
	.description = "Echo test",
	.parameters_json = "{}",
	.execute = mock_echo_execute,
};

static int test_agent_run_with_stub_and_no_tools(void)
{
	const char *path = "/tmp/shellclaw_test_agent_config.toml";
	FILE *f = fopen(path, "w");
	ASSERT(f);
	fprintf(f, "[agent]\nmodel = \"test\"\n");
	fclose(f);
	config_t *cfg = NULL;
	char errbuf[256];
	ASSERT(config_load(path, &cfg, errbuf, sizeof(errbuf)) == 0);
	ASSERT(cfg != NULL);
	const provider_t *provider = provider_stub_get();
	ASSERT(provider != NULL);
	char response_buf[4096];
	int ret = agent_run(cfg, "cli:test", "Hello", provider, NULL, 0, response_buf, sizeof(response_buf));
	ASSERT(ret == 0);
	/* Stub may leave response_buf empty; success is sufficient for 5.1. */
	config_free(cfg);
	remove(path);
	return 0;
}

static int test_agent_run_invalid_args_returns_error(void)
{
	const char *path = "/tmp/shellclaw_test_agent_invalid.toml";
	FILE *f = fopen(path, "w");
	ASSERT(f);
	fprintf(f, "[agent]\nmodel = \"x\"\n");
	fclose(f);
	config_t *cfg = NULL;
	config_load(path, &cfg, NULL, 0);
	const provider_t *provider = provider_stub_get();
	char buf[256];
	ASSERT(agent_run(NULL, "s", "Hi", provider, NULL, 0, buf, sizeof(buf)) != 0);
	ASSERT(agent_run(cfg, NULL, "Hi", provider, NULL, 0, buf, sizeof(buf)) != 0);
	ASSERT(agent_run(cfg, "s", NULL, provider, NULL, 0, buf, sizeof(buf)) != 0);
	ASSERT(agent_run(cfg, "s", "Hi", NULL, NULL, 0, buf, sizeof(buf)) != 0);
	ASSERT(agent_run(cfg, "s", "Hi", provider, NULL, 0, NULL, 256) != 0);
	ASSERT(agent_run(cfg, "s", "Hi", provider, NULL, 0, buf, 0) != 0);
	config_free(cfg);
	remove(path);
	return 0;
}

static int test_context_assembly_system_prompt_history_memories(void)
{
	int failed = 1;
	const char *db_path = "build/test_agent_ctx.db";
	const char *skills_dir = "build/test_agent_skills";
	const char *config_path = "build/test_agent_ctx.toml";
	config_t *cfg = NULL;
	memory_cleanup();
	if (memory_init(db_path) != 0) goto cleanup;
	if (session_save("test_sess", "[{\"role\":\"user\",\"content\":\"old user\"},{\"role\":\"assistant\",\"content\":\"old assistant\"}]") != 0) goto cleanup;
	if (memory_save("pref", "User likes coffee. New message context.", NULL) != 0) goto cleanup;
#ifdef _WIN32
	if (_mkdir(skills_dir) != 0) goto cleanup;
#else
	if (mkdir(skills_dir, 0755) != 0) goto cleanup;
#endif
	char skill_path[512];
	snprintf(skill_path, sizeof(skill_path), "%s/skill.md", skills_dir);
	FILE *sf = fopen(skill_path, "w");
	if (!sf) goto cleanup;
	fprintf(sf, "Test skill line for context.");
	fclose(sf);
	sf = NULL;
	FILE *cf = fopen(config_path, "w");
	if (!cf) goto cleanup;
	fprintf(cf, "[agent]\nmodel = \"test\"\n[memory]\npath = \"%s\"\n[skills]\ndir = \"%s\"\n", db_path, skills_dir);
	fclose(cf);
	cf = NULL;
	char errbuf[256] = {0};
	if (config_load(config_path, &cfg, errbuf, sizeof(errbuf)) != 0) goto cleanup;
	if (!cfg) goto cleanup;
	char response_buf[4096];
	if (agent_run(cfg, "test_sess", "new message", &spy_provider, NULL, 0, response_buf, sizeof(response_buf)) != 0) goto cleanup;
	if (spy_message_count < 4) goto cleanup;
	if (!spy_roles[0] || strcmp(spy_roles[0], "system") != 0) goto cleanup;
	if (!strstr(spy_content[0], "User likes coffee")) goto cleanup;
	if (!strstr(spy_content[0], "Test skill line for context.")) goto cleanup;
	if (strstr(spy_content[0], "local/offline inference") != NULL) goto cleanup;
	if (!strstr(spy_content[1], "old user")) goto cleanup;
	if (!strstr(spy_content[2], "old assistant")) goto cleanup;
	if (!spy_roles[3] || strcmp(spy_roles[3], "user") != 0) goto cleanup;
	if (!strstr(spy_content[3], "new message")) goto cleanup;
	failed = 0;
cleanup:
	config_free(cfg);
	remove(config_path);
	remove(skill_path);
	rmdir(skills_dir);
	remove(db_path);
	memory_cleanup();
	return failed;
}

static int test_react_loop_tool_then_text(void)
{
	int failed = 1;
	const char *path = "build/test_agent_react.toml";
	FILE *f = fopen(path, "w");
	ASSERT(f);
	fprintf(f, "[agent]\nmodel = \"test\"\nmax_tool_iterations = 5\n");
	fclose(f);
	config_t *cfg = NULL;
	char errbuf[256];
	if (config_load(path, &cfg, errbuf, sizeof(errbuf)) != 0) goto cleanup;
	if (cfg == NULL) goto cleanup;
	const agent_tool_t *tools = &mock_echo_tool;
	size_t tool_count = 1;
	char response_buf[4096];
	response_buf[0] = '\0';
	int ret = agent_run(cfg, "cli:react", "hi", &tool_then_text_provider, tools, tool_count, response_buf, sizeof(response_buf));
	if (ret != 0) goto cleanup;
	if (strstr(response_buf, "final text") == NULL) goto cleanup;
	if (tool_then_text_call_count != 2) goto cleanup;
	failed = 0;
cleanup:
	config_free(cfg);
	remove(path);
	return failed;
}

static int test_react_loop_max_iterations(void)
{
	int failed = 1;
	const char *path = "build/test_agent_maxiter.toml";
	FILE *f = fopen(path, "w");
	ASSERT(f);
	fprintf(f, "[agent]\nmodel = \"test\"\nmax_tool_iterations = 2\n");
	fclose(f);
	config_t *cfg = NULL;
	char errbuf[256];
	if (config_load(path, &cfg, errbuf, sizeof(errbuf)) != 0) goto cleanup;
	if (cfg == NULL) goto cleanup;
	const agent_tool_t *tools = &mock_echo_tool;
	size_t tool_count = 1;
	char response_buf[4096];
	response_buf[0] = '\0';
	int ret = agent_run(cfg, "cli:maxiter", "hi", &always_tool_provider, tools, tool_count, response_buf, sizeof(response_buf));
	if (ret != 0) goto cleanup;
	if (strstr(response_buf, "partial") == NULL) goto cleanup;
	/* First chat + 2 tool rounds (max_tool_iterations=2) = 3 provider calls. */
	if (always_tool_call_count != 3) goto cleanup;
	failed = 0;
cleanup:
	config_free(cfg);
	remove(path);
	return failed;
}

static int persist_reply_init(const config_t *cfg) { (void)cfg; return 0; }
static int persist_reply_chat(const provider_message_t *messages, size_t message_count,
	const provider_tool_def_t *tools, size_t tool_count, provider_response_t *response)
{
	(void)messages;
	(void)message_count;
	(void)tools;
	(void)tool_count;
	response->error = 0;
	response->content = strdup("persisted reply");
	response->tool_calls = NULL;
	response->tool_calls_count = 0;
	return 0;
}
static void persist_reply_cleanup(void) {}
static const provider_t persist_reply_provider = {
	.name = "persist_reply",
	.init = persist_reply_init,
	.chat = persist_reply_chat,
	.cleanup = persist_reply_cleanup,
};

static int test_session_persisted_after_exchange(void)
{
	int failed = 1;
	config_t *cfg = NULL;
	const char *db_path = "build/test_agent_persist.db";
	const char *config_path = "build/test_agent_persist.toml";
	memory_cleanup();
	if (memory_init(db_path) != 0) goto cleanup;
	FILE *cf = fopen(config_path, "w");
	ASSERT(cf);
	fprintf(cf, "[agent]\nmodel = \"test\"\n[memory]\npath = \"%s\"\n", db_path);
	fclose(cf);
	char errbuf[256];
	if (config_load(config_path, &cfg, errbuf, sizeof(errbuf)) != 0) goto cleanup;
	if (cfg == NULL) goto cleanup;
	char response_buf[4096];
	response_buf[0] = '\0';
	const char *session_id = "cli:persist";
	const char *user_msg = "user message for persist test";
	int ret = agent_run(cfg, session_id, user_msg, &persist_reply_provider, NULL, 0, response_buf, sizeof(response_buf));
	if (ret != 0) goto cleanup;
	if (strstr(response_buf, "persisted reply") == NULL) goto cleanup;
	char loaded[8192];
	loaded[0] = '\0';
	if (session_load(session_id, loaded, sizeof(loaded)) != 0) goto cleanup;
	if (strstr(loaded, user_msg) == NULL) goto cleanup;
	if (strstr(loaded, "persisted reply") == NULL) goto cleanup;
	failed = 0;
cleanup:
	config_free(cfg);
	remove(config_path);
	remove(db_path);
	memory_cleanup();
	return failed;
}

static int compaction_call_count;
static int compaction_init(const config_t *cfg) { (void)cfg; compaction_call_count = 0; return 0; }
static int compaction_chat(const provider_message_t *messages, size_t message_count,
	const provider_tool_def_t *tools, size_t tool_count, provider_response_t *response)
{
	(void)tools;
	(void)tool_count;
	response->error = 0;
	response->tool_calls = NULL;
	response->tool_calls_count = 0;
	compaction_call_count++;
	if (compaction_call_count == 1) {
		response->content = strdup("Summary of earlier conversation.");
		return 0;
	}
	response->content = strdup("ok");
	spy_roles_clear();
	spy_message_count = message_count;
	for (size_t i = 0; i < message_count && i < SPY_SLOTS; i++) {
		spy_roles[i] = messages[i].role ? strdup(messages[i].role) : NULL;
		if (messages[i].content) {
			size_t n = strlen(messages[i].content);
			if (n >= SPY_CONTENT_SIZE) n = SPY_CONTENT_SIZE - 1;
			memcpy(spy_content[i], messages[i].content, n + 1);
		} else
			spy_content[i][0] = '\0';
	}
	return 0;
}
static void compaction_cleanup(void) {}
static const provider_t compaction_provider = {
	.name = "compaction",
	.init = compaction_init,
	.chat = compaction_chat,
	.cleanup = compaction_cleanup,
};

static int test_context_compaction_when_history_exceeds_max(void)
{
	int failed = 1;
	config_t *cfg = NULL;
	const char *db_path = "build/test_agent_compact.db";
	const char *config_path = "build/test_agent_compact.toml";
	memory_cleanup();
	if (memory_init(db_path) != 0) goto cleanup_early;
	char session_json[65536];
	size_t off = 0;
	off += (size_t)snprintf(session_json + off, sizeof(session_json) - off, "[");
	for (int i = 0; i < 50; i++) {
		const char *role = (i % 2 == 0) ? "user" : "assistant";
		off += (size_t)snprintf(session_json + off, sizeof(session_json) - off,
			"%s{\"role\":\"%s\",\"content\":\"msg_%d\"}", i ? "," : "", role, i);
	}
	off += (size_t)snprintf(session_json + off, sizeof(session_json) - off, "]");
	if (session_save("cli:compact", session_json) != 0) goto cleanup;
	FILE *cf = fopen(config_path, "w");
	if (!cf) goto cleanup;
	fprintf(cf, "[agent]\nmodel = \"test\"\nmax_context_messages = 40\n[memory]\npath = \"%s\"\n", db_path);
	fclose(cf);
	char errbuf[256];
	if (config_load(config_path, &cfg, errbuf, sizeof(errbuf)) != 0) goto cleanup;
	if (cfg == NULL) goto cleanup;
	char response_buf[4096];
	response_buf[0] = '\0';
	int ret = agent_run(cfg, "cli:compact", "hi", &compaction_provider, NULL, 0, response_buf, sizeof(response_buf));
	if (ret != 0) goto cleanup;
	if (compaction_call_count < 2) goto cleanup;
	if (strstr(response_buf, "ok") == NULL) goto cleanup;
	/* Second chat context must contain the summary and tail (e.g. msg_11 from kept raw messages). */
	int found_summary = 0;
	int found_tail = 0;
	for (int i = 0; i < SPY_SLOTS && (i < (int)spy_message_count); i++) {
		if (strstr(spy_content[i], "Summary of earlier") != NULL) found_summary = 1;
		if (strstr(spy_content[i], "msg_11") != NULL) found_tail = 1;
	}
	if (!found_summary) goto cleanup;
	if (!found_tail) goto cleanup;
	failed = 0;
cleanup:
	config_free(cfg);
cleanup_early:
	remove(config_path);
	remove(db_path);
	memory_cleanup();
	return failed;
}

static int test_local_offline_note_when_active_is_local(void)
{
	const char *path = "build/test_agent_local_note.toml";
	FILE *f = fopen(path, "w");
	ASSERT(f);
	fprintf(f, "[agent]\nmodel = \"test\"\n");
	fclose(f);
	config_t *cfg = NULL;
	char errbuf[256];
	ASSERT(config_load(path, &cfg, errbuf, sizeof(errbuf)) == 0);
	char response_buf[4096];
	spy_roles_clear();
	shellclaw_agent_set_test_active_backend_name("local");
	int ret = agent_run(cfg, "cli:localnote", "hey", &spy_provider, NULL, 0, response_buf, sizeof(response_buf));
	shellclaw_agent_set_test_active_backend_name(NULL);
	config_free(cfg);
	remove(path);
	ASSERT(ret == 0);
	ASSERT(spy_message_count >= 1);
	ASSERT(strstr(spy_content[0], "local/offline inference") != NULL);
	return 0;
}

static int provider_error_init(const config_t *cfg) { (void)cfg; return 0; }
static int provider_error_chat(const provider_message_t *messages, size_t message_count,
	const provider_tool_def_t *tools, size_t tool_count, provider_response_t *response)
{
	(void)messages;
	(void)message_count;
	(void)tools;
	(void)tool_count;
	provider_set_error(response, "mock provider error");
	return -1;
}
static void provider_error_cleanup(void) {}
static const provider_t provider_error_provider = {
	.name = "provider_error",
	.init = provider_error_init,
	.chat = provider_error_chat,
	.cleanup = provider_error_cleanup,
};

static int test_agent_provider_error_response(void)
{
	const char *path = "build/test_agent_provider_err.toml";
	FILE *f = fopen(path, "w");
	ASSERT(f);
	fprintf(f, "[agent]\nmodel = \"test\"\n");
	fclose(f);
	config_t *cfg = NULL;
	char errbuf[256];
	char response_buf[4096];
	int ret;
	ASSERT(config_load(path, &cfg, errbuf, sizeof(errbuf)) == 0);
	response_buf[0] = '\0';
	ret = agent_run(cfg, "cli:perr", "hi", &provider_error_provider, NULL, 0,
	                response_buf, sizeof(response_buf));
	ASSERT(ret != 0);
	ASSERT(strstr(response_buf, "mock provider error") != NULL);
	config_free(cfg);
	remove(path);
	return 0;
}

static int unknown_tool_call_count;
static int unknown_tool_init(const config_t *cfg) { (void)cfg; unknown_tool_call_count = 0; return 0; }
static int unknown_tool_chat(const provider_message_t *messages, size_t message_count,
	const provider_tool_def_t *tools, size_t tool_count, provider_response_t *response)
{
	(void)messages;
	(void)message_count;
	(void)tools;
	(void)tool_count;
	response->error = 0;
	response->tool_calls = NULL;
	response->tool_calls_count = 0;
	response->content = NULL;
	unknown_tool_call_count++;
	if (unknown_tool_call_count == 1) {
		response->tool_calls = malloc(sizeof(provider_tool_call_t));
		ASSERT(response->tool_calls != NULL);
		response->tool_calls[0].id = strdup("u1");
		response->tool_calls[0].name = strdup("no_such_tool");
		response->tool_calls[0].arguments = strdup("{}");
		response->tool_calls_count = 1;
		response->content = strdup("");
		return 0;
	}
	response->content = strdup("after unknown tool");
	return 0;
}
static void unknown_tool_cleanup(void) {}
static const provider_t unknown_tool_provider = {
	.name = "unknown_tool",
	.init = unknown_tool_init,
	.chat = unknown_tool_chat,
	.cleanup = unknown_tool_cleanup,
};

static int test_agent_unknown_tool_continues(void)
{
	const char *path = "build/test_agent_unknown_tool.toml";
	FILE *f = fopen(path, "w");
	ASSERT(f);
	fprintf(f, "[agent]\nmodel = \"test\"\nmax_tool_iterations = 5\n");
	fclose(f);
	config_t *cfg = NULL;
	char errbuf[256];
	char response_buf[4096];
	int ret;
	const agent_tool_t *tools = &mock_echo_tool;
	ASSERT(config_load(path, &cfg, errbuf, sizeof(errbuf)) == 0);
	response_buf[0] = '\0';
	unknown_tool_call_count = 0;
	ret = agent_run(cfg, "cli:unk", "hi", &unknown_tool_provider, tools, 1,
	                response_buf, sizeof(response_buf));
	ASSERT(ret == 0);
	ASSERT(strstr(response_buf, "after unknown tool") != NULL);
	ASSERT(unknown_tool_call_count == 2);
	config_free(cfg);
	remove(path);
	return 0;
}

static int seq_tool_exec_count;
static int seq_tool_execute(const char *args_json, char *result_buf, size_t max_len)
{
	(void)args_json;
	seq_tool_exec_count++;
	if (max_len > 0) {
		snprintf(result_buf, max_len, "tool_output_%d", seq_tool_exec_count);
		result_buf[max_len - 1] = '\0';
	}
	return 0;
}
static const agent_tool_t seq_echo_tool = {
	.name = "echo",
	.description = "Echo test with sequence counter",
	.parameters_json = "{}",
	.execute = seq_tool_execute,
};

static int multi_tool_round_call_count;
static int multi_tool_round_saw_live_tool_calls;
static int multi_tool_round_init(const config_t *cfg)
{
	(void)cfg;
	multi_tool_round_call_count = 0;
	seq_tool_exec_count = 0;
	multi_tool_round_saw_live_tool_calls = 0;
	return 0;
}
static int multi_tool_history_is_intact(const provider_message_t *messages, size_t message_count)
{
	size_t i;
	int saw_first_tool_output = 0;
	const char *first_call_id = NULL;
	int saw_matching_use_id = 0;
	for (i = 0; i < message_count; i++) {
		if (messages[i].content && strstr(messages[i].content, "tool_output_1") != NULL)
			saw_first_tool_output = 1;
		if (!first_call_id && messages[i].tool_calls && messages[i].tool_calls_count == 1 &&
		    messages[i].tool_calls[0].id) {
			first_call_id = messages[i].tool_calls[0].id;
			if (first_call_id[0] == '\0')
				return 0;
		}
	}
	if (!saw_first_tool_output || !first_call_id)
		return 0;
	for (i = 0; i < message_count; i++) {
		if (messages[i].tool_use_id && strcmp(messages[i].tool_use_id, first_call_id) == 0) {
			saw_matching_use_id = 1;
			break;
		}
	}
	return saw_matching_use_id;
}
static int multi_tool_round_chat(const provider_message_t *messages, size_t message_count,
	const provider_tool_def_t *tools, size_t tool_count, provider_response_t *response)
{
	(void)tools;
	(void)tool_count;
	response->error = 0;
	response->tool_calls = NULL;
	response->tool_calls_count = 0;
	response->content = NULL;
	multi_tool_round_call_count++;
	if (multi_tool_round_call_count >= 3) {
		if (!multi_tool_history_is_intact(messages, message_count)) {
			response->content = strdup("CORRUPTED_TOOL_HISTORY");
			return 0;
		}
		multi_tool_round_saw_live_tool_calls = 1;
	}
	if (multi_tool_round_call_count <= 2) {
		response->tool_calls = malloc(sizeof(provider_tool_call_t));
		if (!response->tool_calls) {
			response->error = 1;
			return -1;
		}
		response->tool_calls[0].id = strdup("mt1");
		response->tool_calls[0].name = strdup("echo");
		response->tool_calls[0].arguments = strdup("{}");
		response->tool_calls_count = 1;
		response->content = strdup("");
		return 0;
	}
	response->content = strdup("multi tool done");
	return 0;
}
static void multi_tool_round_cleanup(void) {}
static const provider_t multi_tool_round_provider = {
	.name = "multi_tool_round",
	.init = multi_tool_round_init,
	.chat = multi_tool_round_chat,
	.cleanup = multi_tool_round_cleanup,
};

static int test_react_loop_preserves_prior_tool_results(void)
{
	int failed = 1;
	const char *path = "build/test_agent_multi_tool.toml";
	FILE *f = fopen(path, "w");
	ASSERT(f);
	fprintf(f, "[agent]\nmodel = \"test\"\nmax_tool_iterations = 5\n");
	fclose(f);
	config_t *cfg = NULL;
	char errbuf[256];
	char response_buf[4096];
	int ret;
	if (config_load(path, &cfg, errbuf, sizeof(errbuf)) != 0) goto cleanup;
	if (cfg == NULL) goto cleanup;
	multi_tool_round_call_count = 0;
	seq_tool_exec_count = 0;
	multi_tool_round_saw_live_tool_calls = 0;
	response_buf[0] = '\0';
	ret = agent_run(cfg, "cli:multitool", "hi", &multi_tool_round_provider, &seq_echo_tool, 1,
	                response_buf, sizeof(response_buf));
	if (ret != 0) goto cleanup;
	if (strstr(response_buf, "multi tool done") == NULL) goto cleanup;
	if (strstr(response_buf, "CORRUPTED_TOOL_HISTORY") != NULL) goto cleanup;
	if (multi_tool_round_call_count != 3) goto cleanup;
	if (seq_tool_exec_count != 2) goto cleanup;
	if (!multi_tool_round_saw_live_tool_calls) goto cleanup;
	failed = 0;
cleanup:
	config_free(cfg);
	remove(path);
	return failed;
}

static int test_local_offline_note_skipped_for_non_local(void)
{
	const char *path = "build/test_agent_nonlocal_note.toml";
	FILE *f = fopen(path, "w");
	ASSERT(f);
	fprintf(f, "[agent]\nmodel = \"test\"\n");
	fclose(f);
	config_t *cfg = NULL;
	char errbuf[256];
	ASSERT(config_load(path, &cfg, errbuf, sizeof(errbuf)) == 0);
	char response_buf[4096];
	spy_roles_clear();
	shellclaw_agent_set_test_active_backend_name("stub");
	int ret = agent_run(cfg, "cli:nonlocalnote", "hey", &spy_provider, NULL, 0, response_buf, sizeof(response_buf));
	shellclaw_agent_set_test_active_backend_name(NULL);
	config_free(cfg);
	remove(path);
	ASSERT(ret == 0);
	ASSERT(spy_message_count >= 1);
	ASSERT(strstr(spy_content[0], "local/offline inference") == NULL);
	return 0;
}

/* SESSION_JSON_MAX in agent.c is 128 KiB. A reply this large plus a near-full
 * prior session forces append_exchange_to_session_json over the cap. */
#define OVERFLOW_REPLY_BYTES (16 * 1024)

static int overflow_reply_init(const config_t *cfg) { (void)cfg; return 0; }
static int overflow_reply_chat(const provider_message_t *messages, size_t message_count,
	const provider_tool_def_t *tools, size_t tool_count, provider_response_t *response)
{
	char *big;
	(void)messages;
	(void)message_count;
	(void)tools;
	(void)tool_count;
	response->error = 0;
	response->tool_calls = NULL;
	response->tool_calls_count = 0;
	big = malloc(OVERFLOW_REPLY_BYTES);
	if (!big) return -1;
	memset(big, 'R', OVERFLOW_REPLY_BYTES - 1);
	big[OVERFLOW_REPLY_BYTES - 1] = '\0';
	response->content = big;
	return 0;
}
static void overflow_reply_cleanup(void) {}
static const provider_t overflow_reply_provider = {
	.name = "overflow_reply",
	.init = overflow_reply_init,
	.chat = overflow_reply_chat,
	.cleanup = overflow_reply_cleanup,
};

static int keep_history_init(const config_t *cfg) { (void)cfg; return 0; }
static int keep_history_chat(const provider_message_t *messages, size_t message_count,
	const provider_tool_def_t *tools, size_t tool_count, provider_response_t *response)
{
	(void)tools;
	(void)tool_count;
	response->error = 0;
	response->tool_calls = NULL;
	response->tool_calls_count = 0;
	response->content = strdup("ok");
	spy_roles_clear();
	spy_message_count = message_count;
	for (size_t i = 0; i < message_count && i < SPY_SLOTS; i++) {
		spy_roles[i] = messages[i].role ? strdup(messages[i].role) : NULL;
		if (messages[i].content) {
			size_t n = strlen(messages[i].content);
			if (n >= SPY_CONTENT_SIZE) n = SPY_CONTENT_SIZE - 1;
			memcpy(spy_content[i], messages[i].content, n);
			spy_content[i][n] = '\0';
		} else
			spy_content[i][0] = '\0';
	}
	return 0;
}
static void keep_history_cleanup(void) {}
static const provider_t keep_history_provider = {
	.name = "keep_history",
	.init = keep_history_init,
	.chat = keep_history_chat,
	.cleanup = keep_history_cleanup,
};

/**
 * Concrete trigger: near-cap session JSON + fat assistant reply.
 * Before the fix, append truncated mid-JSON, session_save persisted corrupt
 * payload, and the next agent_run parse wiped history. After the fix, overflow
 * refuses to save and prior history remains parseable.
 */
static int test_session_overflow_does_not_corrupt_history(void)
{
	int failed = 1;
	config_t *cfg = NULL;
	const char *db_path = "build/test_agent_overflow.db";
	const char *config_path = "build/test_agent_overflow.toml";
	const char *session_id = "cli:overflow";
	const char *marker = "UNIQUE_HISTORY_MARKER_xyz";
	/* Two large messages keep msg_count under max_context so compaction does not shrink first. */
	enum { PAD_A = 62 * 1024, PAD_B = 62 * 1024, LOAD_CAP = 130 * 1024 };
	char *session_json = NULL;
	char *pad_a = NULL;
	char *pad_b = NULL;
	char *loaded = NULL;
	size_t need;
	size_t off = 0;
	char response_buf[OVERFLOW_REPLY_BYTES + 64];
	cJSON *parsed;

	memory_cleanup();
	if (memory_init(db_path) != 0) goto cleanup;
	pad_a = malloc(PAD_A + 1);
	pad_b = malloc(PAD_B + 1);
	if (!pad_a || !pad_b) goto cleanup;
	memset(pad_a, 'A', PAD_A);
	pad_a[PAD_A] = '\0';
	memset(pad_b, 'B', PAD_B);
	pad_b[PAD_B] = '\0';
	need = strlen(marker) + PAD_A + PAD_B + 128;
	session_json = malloc(need);
	if (!session_json) goto cleanup;
	off = (size_t)snprintf(session_json, need,
		"[{\"role\":\"user\",\"content\":\"%s%s\"},{\"role\":\"assistant\",\"content\":\"%s\"}]",
		marker, pad_a, pad_b);
	if (off == 0 || off >= need) goto cleanup;
	if (session_save(session_id, session_json) != 0) goto cleanup;

	{
		FILE *cf = fopen(config_path, "w");
		if (!cf) goto cleanup;
		fprintf(cf, "[agent]\nmodel = \"test\"\nmax_context_messages = 40\n[memory]\npath = \"%s\"\n",
			db_path);
		fclose(cf);
	}
	{
		char errbuf[256];
		if (config_load(config_path, &cfg, errbuf, sizeof(errbuf)) != 0) goto cleanup;
	}
	if (!cfg) goto cleanup;

	response_buf[0] = '\0';
	if (agent_run(cfg, session_id, "push over the limit", &overflow_reply_provider, NULL, 0,
	              response_buf, sizeof(response_buf)) != 0) {
		fprintf(stderr, "FAIL: tests/test_agent.c: overflow agent_run failed\n");
		goto cleanup;
	}

	loaded = malloc(LOAD_CAP);
	if (!loaded) goto cleanup;
	loaded[0] = '\0';
	if (session_load(session_id, loaded, LOAD_CAP) != 0) goto cleanup;
	parsed = cJSON_Parse(loaded);
	if (!parsed || !cJSON_IsArray(parsed)) {
		if (parsed) cJSON_Delete(parsed);
		fprintf(stderr, "FAIL: tests/test_agent.c: overflow session JSON unparseable\n");
		goto cleanup;
	}
	cJSON_Delete(parsed);
	if (strstr(loaded, marker) == NULL) goto cleanup;

	/* Next turn must still see prior history (not wiped to empty []). */
	spy_roles_clear();
	response_buf[0] = '\0';
	if (agent_run(cfg, session_id, "still there?", &keep_history_provider, NULL, 0,
	              response_buf, sizeof(response_buf)) != 0)
		goto cleanup;
	{
		int found = 0;
		for (size_t j = 0; j < spy_message_count && j < SPY_SLOTS; j++) {
			if (strstr(spy_content[j], marker) != NULL) {
				found = 1;
				break;
			}
		}
		if (!found) goto cleanup;
	}
	failed = 0;
cleanup:
	config_free(cfg);
	free(session_json);
	free(pad_a);
	free(pad_b);
	free(loaded);
	remove(config_path);
	remove(db_path);
	memory_cleanup();
	return failed;
}

/**
 * A stored blob larger than SESSION_JSON_MAX must not be replaced by a later
 * small turn: session_load refuse leaves an empty buffer, and persist must not
 * treat that as a new empty session.
 */
static int test_oversized_stored_session_not_wiped_by_small_turn(void)
{
	int failed = 1;
	config_t *cfg = NULL;
	const char *db_path = "build/test_agent_oversize_load.db";
	const char *config_path = "build/test_agent_oversize_load.toml";
	const char *session_id = "cli:oversize-load";
	const char *marker = "OVERSIZE_LOAD_MARKER_xyz";
	enum { PAD = 128 * 1024, LOAD_CAP = 160 * 1024 };
	char *pad = NULL;
	char *session_json = NULL;
	char *loaded = NULL;
	size_t need;
	size_t off = 0;
	char response_buf[256];
	cJSON *parsed;

	memory_cleanup();
	if (memory_init(db_path) != 0) goto cleanup;
	pad = malloc(PAD + 1);
	if (!pad) goto cleanup;
	memset(pad, 'Z', PAD);
	pad[PAD] = '\0';
	need = strlen(marker) + PAD + 128;
	session_json = malloc(need);
	if (!session_json) goto cleanup;
	off = (size_t)snprintf(session_json, need,
		"[{\"role\":\"user\",\"content\":\"%s%s\"}]", marker, pad);
	if (off == 0 || off >= need) goto cleanup;
	if (session_save(session_id, session_json) != 0) goto cleanup;
	{
		FILE *cf = fopen(config_path, "w");
		if (!cf) goto cleanup;
		fprintf(cf, "[agent]\nmodel = \"test\"\n[memory]\npath = \"%s\"\n", db_path);
		fclose(cf);
	}
	{
		char errbuf[256];
		if (config_load(config_path, &cfg, errbuf, sizeof(errbuf)) != 0) goto cleanup;
	}
	if (!cfg) goto cleanup;
	response_buf[0] = '\0';
	if (agent_run(cfg, session_id, "tiny", &persist_reply_provider, NULL, 0,
	              response_buf, sizeof(response_buf)) != 0) {
		fprintf(stderr, "FAIL: tests/test_agent.c: oversize-load agent_run failed\n");
		goto cleanup;
	}
	loaded = malloc(LOAD_CAP);
	if (!loaded) goto cleanup;
	loaded[0] = '\0';
	if (session_load(session_id, loaded, LOAD_CAP) != 0) goto cleanup;
	parsed = cJSON_Parse(loaded);
	if (!parsed || !cJSON_IsArray(parsed)) {
		if (parsed) cJSON_Delete(parsed);
		fprintf(stderr, "FAIL: tests/test_agent.c: oversize stored session wiped or corrupt\n");
		goto cleanup;
	}
	cJSON_Delete(parsed);
	if (strstr(loaded, marker) == NULL) {
		fprintf(stderr, "FAIL: tests/test_agent.c: oversize stored session missing marker\n");
		goto cleanup;
	}
	failed = 0;
cleanup:
	config_free(cfg);
	free(pad);
	free(session_json);
	free(loaded);
	remove(config_path);
	remove(db_path);
	memory_cleanup();
	return failed;
}

static int write_filled_soul_file(const char *path, size_t nbytes)
{
	char chunk[4096];
	size_t remaining = nbytes;
	FILE *sf = fopen(path, "w");
	if (!sf)
		return -1;
	memset(chunk, 'A', sizeof(chunk));
	while (remaining > 0) {
		size_t n = remaining < sizeof(chunk) ? remaining : sizeof(chunk);
		if (fwrite(chunk, 1, n, sf) != n) {
			fclose(sf);
			return -1;
		}
		remaining -= n;
	}
	fclose(sf);
	return 0;
}

static int write_mem_overflow_config(const char *config_path, const char *soul_path,
	const char *db_path)
{
	FILE *cf = fopen(config_path, "w");
	if (!cf)
		return -1;
	fprintf(cf,
	        "[agent]\nmodel = \"test\"\n[agent.identity]\nsoul = \"%s\"\n"
	        "[memory]\ndb_path = \"%s\"\n[skills]\ndir = \"build/test_agent_mem_noskills\"\n",
	        soul_path, db_path);
	fclose(cf);
	return 0;
}

static int prepare_coffee_memory_store(const char *db_path)
{
	char recall_check[512];
	memory_cleanup();
	if (memory_init(db_path) != 0)
		return -1;
	if (memory_save("pref", "User likes coffee. New message context.", NULL) != 0)
		return -1;
	if (memory_recall("coffee", recall_check, sizeof(recall_check), 5) != 0)
		return -1;
	if (recall_check[0] == '\0')
		return -1;
	return 0;
}

static int test_full_system_prompt_skips_memory_append_without_overflow(void)
{
	int failed = 1;
	const char *db_path = "build/test_agent_mem_overflow.db";
	const char *soul_path = "build/test_agent_mem_overflow_soul.md";
	const char *config_path = "build/test_agent_mem_overflow.toml";
	const char *err_path = "build/test_agent_mem_overflow.err";
	config_t *cfg = NULL;
	char response_buf[4096];
	char errbuf[256] = {0};
	char captured[2048];
	int saved_stderr = -1;
	int errfd = -1;
	FILE *ef;

	/* SYSTEM_PROMPT_MAX is 65536; a 65535-byte SOUL fills it so the 22-byte
	 * "Relevant memories" prefix cannot fit. The old clamp still memcpy'd
	 * the prefix past the heap allocation (Refs: #75). */
	if (prepare_coffee_memory_store(db_path) != 0)
		goto cleanup;
	if (write_filled_soul_file(soul_path, 65535U) != 0)
		goto cleanup;
	if (write_mem_overflow_config(config_path, soul_path, db_path) != 0)
		goto cleanup;
	if (config_load(config_path, &cfg, errbuf, sizeof(errbuf)) != 0)
		goto cleanup;
	if (!cfg)
		goto cleanup;
	spy_roles_clear();
	spy_first_content_len = 0;
	errfd = open(err_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (errfd < 0)
		goto cleanup;
	saved_stderr = dup(STDERR_FILENO);
	if (saved_stderr < 0)
		goto cleanup;
	if (dup2(errfd, STDERR_FILENO) < 0)
		goto cleanup;
	close(errfd);
	errfd = -1;
	if (agent_run(cfg, "cli:memoverflow", "coffee", &spy_provider, NULL, 0,
	              response_buf, sizeof(response_buf)) != 0) {
		if (saved_stderr >= 0) {
			fflush(stderr);
			dup2(saved_stderr, STDERR_FILENO);
			close(saved_stderr);
			saved_stderr = -1;
		}
		fprintf(stderr, "FAIL: tests/test_agent.c: full-prompt memory skip agent_run failed\n");
		goto cleanup;
	}
	fflush(stderr);
	dup2(saved_stderr, STDERR_FILENO);
	close(saved_stderr);
	saved_stderr = -1;
	if (spy_message_count < 1)
		goto cleanup;
	if (!spy_roles[0] || strcmp(spy_roles[0], "system") != 0)
		goto cleanup;
	if (spy_first_content_len != 65535U) {
		fprintf(stderr,
		        "FAIL: tests/test_agent.c: system prompt len %zu (expected 65535, memories not skipped)\n",
		        spy_first_content_len);
		goto cleanup;
	}
	if (spy_content[0][0] != 'A')
		goto cleanup;
	ef = fopen(err_path, "r");
	if (!ef)
		goto cleanup;
	{
		size_t n = fread(captured, 1, sizeof(captured) - 1, ef);
		captured[n] = '\0';
		fclose(ef);
	}
	if (strstr(captured, "agent: skip memory injection") == NULL) {
		fprintf(stderr, "FAIL: tests/test_agent.c: missing skip memory injection log\n");
		goto cleanup;
	}
	failed = 0;
cleanup:
	if (errfd >= 0)
		close(errfd);
	if (saved_stderr >= 0) {
		fflush(stderr);
		dup2(saved_stderr, STDERR_FILENO);
		close(saved_stderr);
	}
	config_free(cfg);
	remove(config_path);
	remove(soul_path);
	remove(db_path);
	remove(err_path);
	memory_cleanup();
	return failed;
}

static int test_near_full_system_prompt_keeps_one_recall_byte(void)
{
	int failed = 1;
	const char *db_path = "build/test_agent_mem_trunc.db";
	const char *soul_path = "build/test_agent_mem_trunc_soul.md";
	const char *config_path = "build/test_agent_mem_trunc.toml";
	config_t *cfg = NULL;
	char response_buf[4096];
	char errbuf[256] = {0};

	/* Prefix is 22 bytes. Soul 65510 plus PROMPT_SEP "\\n\\n" yields len 65512
	 * so len + prefix + 1 recall byte + NUL == SYSTEM_PROMPT_MAX. One FTS byte
	 * ('U' from "User likes coffee") must be kept (Refs: #75). */
	if (prepare_coffee_memory_store(db_path) != 0)
		goto cleanup;
	if (write_filled_soul_file(soul_path, 65510U) != 0)
		goto cleanup;
	if (write_mem_overflow_config(config_path, soul_path, db_path) != 0)
		goto cleanup;
	if (config_load(config_path, &cfg, errbuf, sizeof(errbuf)) != 0)
		goto cleanup;
	if (!cfg)
		goto cleanup;
	spy_roles_clear();
	spy_first_content_len = 0;
	if (agent_run(cfg, "cli:memtrunc", "coffee", &spy_provider, NULL, 0,
	              response_buf, sizeof(response_buf)) != 0) {
		fprintf(stderr, "FAIL: tests/test_agent.c: near-full memory clip agent_run failed\n");
		goto cleanup;
	}
	if (spy_message_count < 1)
		goto cleanup;
	if (!spy_roles[0] || strcmp(spy_roles[0], "system") != 0)
		goto cleanup;
	if (spy_first_content_len != 65535U) {
		fprintf(stderr,
		        "FAIL: tests/test_agent.c: clipped prompt len %zu (expected 65535)\n",
		        spy_first_content_len);
		goto cleanup;
	}
	if (spy_first_last_char != 'U') {
		fprintf(stderr,
		        "FAIL: tests/test_agent.c: last byte 0x%02x (expected clipped recall 'U')\n",
		        (unsigned char)spy_first_last_char);
		goto cleanup;
	}
	failed = 0;
cleanup:
	config_free(cfg);
	remove(config_path);
	remove(soul_path);
	remove(db_path);
	memory_cleanup();
	return failed;
}

int main(void)
{
	RUN(test_agent_run_with_stub_and_no_tools());
	RUN(test_agent_run_invalid_args_returns_error());
	RUN(test_context_assembly_system_prompt_history_memories());
	RUN(test_react_loop_tool_then_text());
	RUN(test_react_loop_max_iterations());
	RUN(test_react_loop_preserves_prior_tool_results());
	RUN(test_session_persisted_after_exchange());
	RUN(test_context_compaction_when_history_exceeds_max());
	RUN(test_local_offline_note_when_active_is_local());
	RUN(test_local_offline_note_skipped_for_non_local());
	RUN(test_session_overflow_does_not_corrupt_history());
	RUN(test_oversized_stored_session_not_wiped_by_small_turn());
	RUN(test_full_system_prompt_skips_memory_append_without_overflow());
	RUN(test_near_full_system_prompt_keeps_one_recall_byte());
	RUN(test_agent_provider_error_response());
	RUN(test_agent_unknown_tool_continues());
	printf("test_agent: all tests passed\n");
	return 0;
}
