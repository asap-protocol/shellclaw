/**
 * @file bootstrap.c
 * @brief Subsystem init/cleanup and channel registration.
 */
#define _POSIX_C_SOURCE 200809L

#include "core/bootstrap.h"
#include "core/memory.h"
#include "core/skill.h"
#include "channels/heartbeat.h"
#include "tools/cron.h"
#ifdef SHELLCLAW_GATEWAY
#include "channels/webchat.h"
#include "gateway/auth.h"
#include "gateway/http.h"
#include "gateway/ws.h"
#include "asap/manifest_keys.h"
#endif
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define SKILLS_BUF_SIZE (256 * 1024)
#define SYSTEM_PROMPT_BUF_SIZE (256 * 1024)
#define MAX_CHANNELS 8

static int g_verbose;
static const char *g_cli_one_shot;
static const char *g_config_path;
static config_t *g_cfg;
static const provider_t *g_provider;
static const tool_t *g_tools[SHELLCLAW_MAX_TOOLS];
static size_t g_tool_count;
static const channel_t *g_channels[MAX_CHANNELS];
static int g_channel_count;
#ifdef SHELLCLAW_GATEWAY
static auth_ctx_t *g_auth_ctx;
#endif

void bootstrap_set_verbose(int verbose)
{
	g_verbose = verbose ? 1 : 0;
}

void bootstrap_set_cli_one_shot(const char *one_shot)
{
	g_cli_one_shot = one_shot;
}

void bootstrap_set_config_path(const char *path)
{
	g_config_path = path;
}

const char *bootstrap_get_config_path(void)
{
	return g_config_path;
}

config_t *bootstrap_get_cfg(void)
{
	return g_cfg;
}

void bootstrap_set_cfg(config_t *cfg)
{
	g_cfg = cfg;
}

const provider_t *bootstrap_get_provider(void)
{
	return g_provider;
}

int bootstrap_channel_count(void)
{
	return g_channel_count;
}

const channel_t *bootstrap_channel_at(int index)
{
	if (index < 0 || index >= g_channel_count)
		return NULL;
	return g_channels[index];
}

size_t bootstrap_tool_count(void)
{
	return g_tool_count;
}

const tool_t *bootstrap_tool_at(size_t index)
{
	if (index >= g_tool_count)
		return NULL;
	return g_tools[index];
}

size_t bootstrap_fill_agent_tools(agent_tool_t *out, size_t cap)
{
	size_t tool_count = g_tool_count;
	size_t i;
	if (!out || cap == 0)
		return 0;
	if (tool_count > cap)
		tool_count = cap;
	for (i = 0; i < tool_count; i++) {
		const tool_t *t = g_tools[i];
		if (!t) {
			tool_count = i;
			break;
		}
		out[i].name = t->name;
		out[i].description = t->description;
		out[i].parameters_json = t->parameters_json;
		out[i].execute = t->execute;
	}
	return tool_count;
}

static int memory_init_from_config(const config_t *cfg)
{
	const char *path = config_memory_db_path(cfg);
	return path ? memory_init(path) : -1;
}

static int skills_init(const config_t *cfg)
{
	char *skills_buf;
	char *system_buf;
	int ret;
	if (!cfg) return -1;
	if (config_skills_dir(cfg) && config_skills_dir(cfg)[0])
		(void)skill_watch_start(cfg, g_verbose);
	skills_buf = malloc(SKILLS_BUF_SIZE);
	system_buf = malloc(SYSTEM_PROMPT_BUF_SIZE);
	if (!skills_buf || !system_buf) {
		free(skills_buf);
		free(system_buf);
		return -1;
	}
	ret = skill_load_all(cfg, skills_buf, SKILLS_BUF_SIZE);
	if (ret == 0)
		ret = skill_build_system_prompt_base(cfg, skills_buf, system_buf, SYSTEM_PROMPT_BUF_SIZE);
	free(skills_buf);
	free(system_buf);
	return ret;
}

static void skills_cleanup(void)
{
	skill_watch_stop();
}

static int providers_init(const config_t *cfg)
{
	g_provider = provider_router_get(cfg);
	if (!g_provider) return -1;
	return g_provider->init(cfg);
}

static void providers_cleanup(void)
{
	if (g_provider && g_provider->cleanup)
		g_provider->cleanup();
	g_provider = NULL;
}

static int channels_init(config_t *cfg)
{
	g_cfg = cfg;
	g_channel_count = 0;
	channel_cli_set_one_shot(g_cli_one_shot);
	channel_cli_set_verbose(g_verbose);
	const channel_t *cli = channel_cli_get();
	if (cli->init(cfg) != 0) return -1;
	channel_register("cli", cli);
	g_channels[g_channel_count++] = cli;
	if (config_telegram_enabled(cfg)) {
		const channel_t *tg = channel_telegram_get();
		if (tg->init(cfg) == 0) {
			channel_register("telegram", tg);
			g_channels[g_channel_count++] = tg;
		} else {
			fprintf(stderr, "shellclaw: telegram channel init failed\n");
		}
	}
	if (config_discord_enabled(cfg)) {
		const channel_t *dis = channel_discord_get();
		if (dis->init(cfg) == 0) {
			channel_register("discord", dis);
			g_channels[g_channel_count++] = dis;
		} else {
			fprintf(stderr, "shellclaw: discord channel init failed\n");
		}
	}
#ifdef SHELLCLAW_GATEWAY
	if (config_gateway_enabled(cfg)) {
		const channel_t *wc = channel_webchat_get();
		if (wc->init(cfg) == 0) {
			channel_register("webchat", wc);
			g_channels[g_channel_count++] = wc;
		} else {
			fprintf(stderr, "shellclaw: webchat channel init failed\n");
		}
	}
#endif
	const channel_t *cron_ch = channel_cron_get();
	if (cron_ch->init(cfg) == 0) {
		channel_register("cron", cron_ch);
		g_channels[g_channel_count++] = cron_ch;
	} else {
		fprintf(stderr, "shellclaw: cron channel init failed\n");
	}
	if (config_heartbeat_enabled(cfg)) {
		const channel_t *hb_ch = channel_heartbeat_get();
		if (hb_ch->init(cfg) == 0) {
			channel_register("heartbeat", hb_ch);
			g_channels[g_channel_count++] = hb_ch;
		} else {
			fprintf(stderr, "shellclaw: heartbeat channel init failed\n");
		}
	}
	return 0;
}

static void channels_cleanup(void)
{
	for (int i = 0; i < g_channel_count; i++) {
		if (g_channels[i] && g_channels[i]->cleanup)
			g_channels[i]->cleanup();
	}
	g_channel_count = 0;
	g_cfg = NULL;
}

static int workspace_is_real_dir(const char *workspace)
{
	struct stat st;

	if (lstat(workspace, &st) != 0) {
		fprintf(stderr, "shellclaw: workspace %s: %s\n", workspace, strerror(errno));
		return 0;
	}
	if (S_ISLNK(st.st_mode)) {
		fprintf(stderr, "shellclaw: workspace %s is a symlink\n", workspace);
		return 0;
	}
	if (!S_ISDIR(st.st_mode)) {
		fprintf(stderr, "shellclaw: workspace %s is not a directory\n", workspace);
		return 0;
	}
	return 1;
}

static int ensure_workspace_directory(const char *workspace)
{
	const char *slash;

	if (!workspace || !workspace[0]) return 0;
	slash = strrchr(workspace, '/');
	if (slash && slash != workspace) {
		char parent[PATH_MAX];
		size_t parent_len = (size_t)(slash - workspace);
		if (parent_len < sizeof(parent)) {
			memcpy(parent, workspace, parent_len);
			parent[parent_len] = '\0';
			if (mkdir(parent, 0700) != 0 && errno != EEXIST)
				fprintf(stderr, "shellclaw: mkdir %s: %s\n",
				        parent, strerror(errno));
		}
	}
	if (mkdir(workspace, 0700) != 0 && errno != EEXIST)
		fprintf(stderr, "shellclaw: mkdir workspace %s: %s\n",
		        workspace, strerror(errno));
	if (!workspace_is_real_dir(workspace))
		return -1;
	return 0;
}

int tools_init(const config_t *cfg)
{
	if (ensure_workspace_directory(config_workspace_path(cfg)) != 0)
		return -1;
	tool_set_config(cfg);
	g_tool_count = tool_get_all(g_tools, SHELLCLAW_MAX_TOOLS);
	return 0;
}

static void tools_cleanup(void)
{
	g_tool_count = 0;
}

int init_subsystems(config_t *cfg)
{
	if (memory_init_from_config(cfg) != 0) {
		fprintf(stderr, "Error: memory init failed\n");
		return -1;
	}
	if (skills_init(cfg) != 0) {
		fprintf(stderr, "Error: skills init failed\n");
		memory_cleanup();
		return -1;
	}
	if (providers_init(cfg) != 0) {
		fprintf(stderr, "Error: provider init failed (check default_provider and API keys)\n");
		skills_cleanup();
		memory_cleanup();
		return -1;
	}
	if (channels_init(cfg) != 0) {
		fprintf(stderr, "Error: channels init failed\n");
		providers_cleanup();
		skills_cleanup();
		memory_cleanup();
		return -1;
	}
#ifdef SHELLCLAW_GATEWAY
	if (config_gateway_enabled(cfg)) {
		g_auth_ctx = auth_init(NULL);
		if (!g_auth_ctx) {
			fprintf(stderr, "Error: auth init failed\n");
			channels_cleanup();
			providers_cleanup();
			skills_cleanup();
			memory_cleanup();
			return -1;
		}
		char *code = auth_get_or_create_pairing_code(g_auth_ctx);
		if (code) {
			free(code);
		}
		/* Create/load signing keys eagerly at gateway startup so the FIRST
		 * unauthenticated /.well-known/asap/manifest.json request never
		 * triggers keypair creation + disk fsync (DoS surface). The gateway
		 * must not start if it cannot sign manifests. */
		{
			char keys_err[256] = {0};

			if (manifest_keys_ensure_loaded(keys_err, sizeof(keys_err)) != 0) {
				fprintf(stderr, "shellclaw: signing keys unavailable: %s\n",
					keys_err[0] ? keys_err : "unknown error");
				auth_cleanup(g_auth_ctx);
				g_auth_ctx = NULL;
				channels_cleanup();
				providers_cleanup();
				skills_cleanup();
				memory_cleanup();
				return -1;
			}
		}
		if (http_start(cfg, g_auth_ctx, g_config_path) != 0) {
			fprintf(stderr, "Error: gateway start failed\n");
			auth_cleanup(g_auth_ctx);
			g_auth_ctx = NULL;
			channels_cleanup();
			providers_cleanup();
			skills_cleanup();
			memory_cleanup();
			return -1;
		}
		provider_router_set_status_changed_callback(http_emit_ws_provider_status);
	}
#endif
	/* cppcheck-suppress knownConditionTrueFalse */
	if (tools_init(cfg) != 0) {
		fprintf(stderr, "Error: tools init failed\n");
#ifdef SHELLCLAW_GATEWAY
		http_stop();
		if (g_auth_ctx) { auth_cleanup(g_auth_ctx); g_auth_ctx = NULL; }
#endif
		channels_cleanup();
		providers_cleanup();
		skills_cleanup();
		memory_cleanup();
		return -1;
	}
	return 0;
}

void cleanup_subsystems(void)
{
#ifdef SHELLCLAW_GATEWAY
	/* HTTP/WS callbacks still call auth_validate_token(ctx->auth). Join the
	 * lws thread before freeing auth_ctx (same order as tools_init failure). */
	ws_shutdown_signal();
	http_stop();
	if (g_auth_ctx) {
		auth_cleanup(g_auth_ctx);
		g_auth_ctx = NULL;
	}
	ws_cleanup();
#endif
	tools_cleanup();
	channels_cleanup();
	providers_cleanup();
	skills_cleanup();
	memory_cleanup();
}
