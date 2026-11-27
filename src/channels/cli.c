/**
 * @file cli.c
 * @brief CLI channel: stdin/stdout, -m one-shot, interactive prompt.
 */
#define _POSIX_C_SOURCE 200809L

#include "channels/channel.h"
#include "core/config.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <unistd.h>

#define CLI_SESSION_ID "cli:default"
#define CLI_USER_ID    "default"
#define LINE_BUF_SIZE  8192

static char *g_one_shot;
static int g_verbose;
static int g_one_shot_mode;
static char g_input[LINE_BUF_SIZE - 1];
static size_t g_input_length;
static int g_prompt_written;
static int g_input_closed;

void channel_cli_set_one_shot(const char *msg)
{
	free(g_one_shot);
	g_one_shot = msg ? strdup(msg) : NULL;
}

void channel_cli_set_verbose(int v)
{
	g_verbose = v;
}

static int cli_init(const config_t *cfg)
{
	(void)cfg;
	g_one_shot_mode = g_one_shot != NULL;
	g_input_length = 0;
	g_prompt_written = 0;
	g_input_closed = 0;
	return 0;
}

static int cli_message(channel_incoming_msg_t *out, const char *text)
{
	out->session_id = strdup(CLI_SESSION_ID);
	out->user_id = strdup(CLI_USER_ID);
	out->text = strdup(text);
	if (!out->session_id || !out->user_id || !out->text) {
		channel_incoming_msg_clear(out);
		return -1;
	}
	return 1;
}

static int cli_read_input(int timeout_ms)
{
	fd_set rfds;
	struct timeval tv;
	FD_ZERO(&rfds);
	FD_SET(STDIN_FILENO, &rfds);
	tv.tv_sec = timeout_ms / 1000;
	tv.tv_usec = (timeout_ms % 1000) * 1000;
	int result = select(STDIN_FILENO + 1, &rfds, NULL, NULL, &tv);
	if (result == 0 || (result < 0 && errno == EINTR))
		return 0;
	if (result < 0) {
		g_input_closed = 1;
		return 1;
	}

	ssize_t count = read(STDIN_FILENO, g_input + g_input_length, sizeof(g_input) - g_input_length);
	if (count < 0 && (errno == EINTR || errno == EAGAIN))
		return 0;
	if (count <= 0)
		g_input_closed = 1;
	else
		g_input_length += (size_t)count;
	return 1;
}

static size_t cli_line_length(void)
{
	const char *newline = memchr(g_input, '\n', g_input_length);
	if (newline)
		return (size_t)(newline - g_input) + 1U;
	return g_input_closed || g_input_length == sizeof(g_input) ? g_input_length : 0;
}

static int cli_poll(channel_incoming_msg_t *out, int timeout_ms)
{
	if (!out) return -1;
	memset(out, 0, sizeof(*out));
	if (g_one_shot_mode) {
		if (!g_one_shot)
			return 0;
		int result = cli_message(out, g_one_shot);
		free(g_one_shot);
		g_one_shot = NULL;
		return result;
	}
	if (!g_input_closed && !g_prompt_written && isatty(STDIN_FILENO)) {
		fputs("You: ", stdout);
		fflush(stdout);
		g_prompt_written = 1;
	}

	size_t length = cli_line_length();
	if (!length && !g_input_closed && cli_read_input(timeout_ms))
		length = cli_line_length();
	if (!length)
		return g_input_closed ? CHANNEL_POLL_CLOSED : 0;

	char line[LINE_BUF_SIZE];
	memcpy(line, g_input, length);
	g_input_length -= length;
	memmove(g_input, g_input + length, g_input_length);
	while (length > 0 && (line[length - 1] == '\n' || line[length - 1] == '\r'))
		length--;
	line[length] = '\0';
	g_prompt_written = 0;
	if (strcmp(line, "/quit") == 0) {
		g_input_length = 0;
		g_input_closed = 1;
		return CHANNEL_POLL_CLOSED;
	}
	if (strcmp(line, "/help") == 0) {
		puts("/status Show the provider status.\n"
		     "/reset Clear this conversation.\n"
		     "/quit Close terminal input.");
		fflush(stdout);
		return 0;
	}
	return cli_message(out, line);
}

static int cli_send(const char *recipient, const char *text,
                    const channel_attachment_t *attachments, size_t att_count)
{
	(void)recipient;
	(void)attachments;
	(void)att_count;
	if (!text) return -1;
	if (g_verbose)
		fprintf(stdout, "\033[36mAgent:\033[0m %s\n", text);
	else
		fprintf(stdout, "%s\n", text);
	fflush(stdout);
	return 0;
}

static void cli_cleanup(void)
{
	free(g_one_shot);
	g_one_shot = NULL;
}

static const channel_t cli_channel = {
	.name = "cli",
	.init = cli_init,
	.poll = cli_poll,
	.send = cli_send,
	.cleanup = cli_cleanup,
};

const channel_t *channel_cli_get(void)
{
	return &cli_channel;
}
