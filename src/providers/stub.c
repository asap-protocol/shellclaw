/**
 * @file stub.c
 * @brief Stub provider for tests and verification of provider_t vtable.
 */
#define _POSIX_C_SOURCE 200809L

#include "providers/provider.h"
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

static int stub_init(const config_t *cfg)
{
	(void)cfg;
	return 0;
}

static int stub_chat(const provider_message_t *messages, size_t message_count,
                    const provider_tool_def_t *tools, size_t tool_count,
                    provider_response_t *response)
{
	(void)messages;
	(void)message_count;
	(void)tools;
	(void)tool_count;
	if (response) {
		response->error = 0;
		response->content = strdup("");
		response->tool_calls = NULL;
		response->tool_calls_count = 0;
	}
	return 0;
}

static void stub_cleanup(void) {}

static const provider_t stub_provider = {
	.name = "stub",
	.init = stub_init,
	.chat = stub_chat,
	.cleanup = stub_cleanup,
};

const provider_t *provider_stub_get(void)
{
	return &stub_provider;
}

static int s_stub_b_chat_should_fail;
static const char *s_stub_b_error_message;

static int stub_b_init(const config_t *cfg)
{
	(void)cfg;
	s_stub_b_chat_should_fail = 0;
	s_stub_b_error_message = NULL;
	return 0;
}

static int stub_b_chat(const provider_message_t *messages, size_t message_count,
                      const provider_tool_def_t *tools, size_t tool_count,
                      provider_response_t *response)
{
	if (s_stub_b_chat_should_fail) {
		const char *err = s_stub_b_error_message;
		if (err == NULL || err[0] == '\0')
			err = "Connection refused (stub-b)";
		provider_set_error(response, err);
		return -1;
	}
	return stub_chat(messages, message_count, tools, tool_count, response);
}

static void stub_b_cleanup(void)
{
	s_stub_b_chat_should_fail = 0;
	s_stub_b_error_message = NULL;
}

static const provider_t stub_b_provider = {
	.name = "stub-b",
	.init = stub_b_init,
	.chat = stub_b_chat,
	.cleanup = stub_b_cleanup,
};

const provider_t *provider_stub_b_get(void)
{
	return &stub_b_provider;
}

void provider_stub_b_set_chat_should_fail(int should_fail)
{
	s_stub_b_chat_should_fail = should_fail ? 1 : 0;
}

void provider_stub_b_set_chat_error_message(const char *message)
{
	s_stub_b_error_message = message;
}
