/**
 * @file reload.c
 * @brief SIGHUP config reload and stale config queue.
 */
#define _POSIX_C_SOURCE 200809L

#include "core/reload.h"
#include "core/bootstrap.h"
#include "channels/channel.h"
#include "channels/heartbeat.h"
#include "providers/provider.h"
#include "tools/tool.h"
#ifdef SHELLCLAW_GATEWAY
#include "gateway/http.h"
#endif
#include <stdio.h>
#include <stdlib.h>

volatile sig_atomic_t g_reload_requested;

typedef struct stale_config_node {
	config_t *cfg;
	struct stale_config_node *next;
} stale_config_node_t;

static stale_config_node_t *g_stale_cfg_head;

void on_hup(int sig)
{
	(void)sig;
	g_reload_requested = 1;
}

int stale_enqueue(config_t *old)
{
	stale_config_node_t *node;
	if (!old)
		return 0;
	node = malloc(sizeof *node);
	if (!node)
		return -1;
	node->cfg = old;
	node->next = g_stale_cfg_head;
	g_stale_cfg_head = node;
	return 0;
}

void stale_free_all(void)
{
	stale_config_node_t *walk;
	stale_config_node_t *nx;
	walk = g_stale_cfg_head;
	while (walk) {
		nx = walk->next;
		config_free(walk->cfg);
		free(walk);
		walk = nx;
	}
	g_stale_cfg_head = NULL;
}

int try_config_reload(config_t **pcfg)
{
	config_t *old;
	config_t *new_cfg;
	char errbuf[256];
	const char *config_path = bootstrap_get_config_path();
	if (!pcfg || !*pcfg || !config_path)
		return -1;
	/* Dashboard PUT may reload from the HTTP thread while main still holds the
	 * previous pointer. Always enqueue the live bootstrap cfg, not the caller's
	 * possibly-stale copy, so a later SIGHUP does not double-free. */
	old = bootstrap_get_cfg();
	if (!old)
		old = *pcfg;
	if (!old)
		return -1;
	new_cfg = NULL;
	if (config_load(config_path, &new_cfg, errbuf, sizeof(errbuf)) != 0) {
		fprintf(stderr, "shellclaw: SIGHUP config reload failed: %s\n",
		        errbuf[0] ? errbuf : "unknown error");
		return -1;
	}
	if (stale_enqueue(old) != 0) {
		fprintf(stderr, "shellclaw: SIGHUP config reload failed: out of memory\n");
		config_free(new_cfg);
		return -1;
	}
	provider_router_set_live_config(new_cfg);
	provider_openai_set_live_config(new_cfg);
	provider_local_set_live_config(new_cfg);
	tool_set_config(new_cfg);
	heartbeat_set_live_config(new_cfg);
	shellclaw_telegram_set_live_cfg(new_cfg);
	shellclaw_discord_set_live_cfg(new_cfg);
#ifdef SHELLCLAW_GATEWAY
	http_set_live_config(new_cfg);
#endif
	*pcfg = new_cfg;
	bootstrap_set_cfg(new_cfg);
	fprintf(stderr, "shellclaw: config reloaded from %s\n", config_path);
	return 0;
}
