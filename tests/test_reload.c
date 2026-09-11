/**
 * @file test_reload.c
 * @brief Unit tests for SIGHUP config reload and stale config queue.
 */

#include "test_runner.h"
#include "core/reload.h"
#include "core/bootstrap.h"
#include "core/config.h"
#include <stdio.h>
#include <string.h>

static config_t *load_minimal_config(const char *path, const char *model)
{
	FILE *f;
	config_t *cfg = NULL;
	char errbuf[256];

	f = fopen(path, "w");
	if (!f)
		return NULL;
	fprintf(f, "[agent]\nmodel = \"%s\"\n", model);
	fclose(f);
	if (config_load(path, &cfg, errbuf, sizeof(errbuf)) != 0) {
		config_free(cfg);
		return NULL;
	}
	return cfg;
}

static config_t *load_config_with_tokens(const char *path, const char *model, int max_tokens)
{
	FILE *f;
	config_t *cfg = NULL;
	char errbuf[256];

	f = fopen(path, "w");
	if (!f)
		return NULL;
	fprintf(f, "[agent]\nmodel = \"%s\"\nmax_tokens = %d\n", model, max_tokens);
	fclose(f);
	if (config_load(path, &cfg, errbuf, sizeof(errbuf)) != 0) {
		config_free(cfg);
		return NULL;
	}
	return cfg;
}

static int test_on_hup_sets_reload_flag(void)
{
	g_reload_requested = 0;
	on_hup(SIGHUP);
	ASSERT(g_reload_requested == 1);
	g_reload_requested = 0;
	return 0;
}

static int test_stale_enqueue_null_is_noop(void)
{
	stale_free_all();
	ASSERT(stale_enqueue(NULL) == 0);
	stale_free_all();
	return 0;
}

static int test_stale_enqueue_preserves_old_config(void)
{
	char path[128];
	config_t *old_cfg;
	config_t *peek_old;

	ASSERT(test_runner_mkstemp_path("shellclaw_test_reload", path, sizeof(path)) == 0);
	old_cfg = load_minimal_config(path, "stale-model");
	ASSERT(old_cfg != NULL);
	ASSERT(strcmp(config_agent_model(old_cfg), "stale-model") == 0);
	ASSERT(stale_enqueue(old_cfg) == 0);
	peek_old = old_cfg;
	old_cfg = load_minimal_config(path, "live-model");
	ASSERT(old_cfg != NULL);
	ASSERT(strcmp(config_agent_model(old_cfg), "live-model") == 0);
	ASSERT(strcmp(config_agent_model(peek_old), "stale-model") == 0);
	config_free(old_cfg);
	stale_free_all();
	remove(path);
	return 0;
}

static int test_stale_enqueue_preserves_independent_configs(void)
{
	char path[128];
	config_t *stale_a;
	config_t *stale_b;
	config_t *live;

	ASSERT(test_runner_mkstemp_path("shellclaw_test_reload", path, sizeof(path)) == 0);
	stale_a = load_config_with_tokens(path, "stale-a", 100);
	ASSERT(stale_a != NULL);
	stale_b = load_config_with_tokens(path, "stale-b", 200);
	ASSERT(stale_b != NULL);
	live = load_config_with_tokens(path, "live-model", 300);
	ASSERT(live != NULL);
	stale_free_all();
	ASSERT(stale_enqueue(stale_a) == 0);
	ASSERT(stale_enqueue(stale_b) == 0);
	ASSERT(strcmp(config_agent_model(stale_a), "stale-a") == 0);
	ASSERT(config_agent_max_tokens(stale_a) == 100);
	ASSERT(strcmp(config_agent_model(stale_b), "stale-b") == 0);
	ASSERT(config_agent_max_tokens(stale_b) == 200);
	ASSERT(strcmp(config_agent_model(live), "live-model") == 0);
	ASSERT(config_agent_max_tokens(live) == 300);
	config_free(live);
	stale_free_all();
	remove(path);
	return 0;
}

static int test_try_config_reload_swaps_live_config(void)
{
	char path[128];
	config_t *cfg = NULL;

	ASSERT(test_runner_mkstemp_path("shellclaw_test_reload", path, sizeof(path)) == 0);
	cfg = load_minimal_config(path, "before-reload");
	ASSERT(cfg != NULL);
	bootstrap_set_config_path(path);
	bootstrap_set_cfg(cfg);
	ASSERT(strcmp(config_agent_model(cfg), "before-reload") == 0);
	{
		FILE *f = fopen(path, "w");
		ASSERT(f);
		fprintf(f, "[agent]\nmodel = \"after-reload\"\n");
		fclose(f);
	}
	try_config_reload(&cfg);
	ASSERT(cfg != NULL);
	ASSERT(strcmp(config_agent_model(cfg), "after-reload") == 0);
	ASSERT(strcmp(config_agent_model(bootstrap_get_cfg()), "after-reload") == 0);
	stale_free_all();
	config_free(cfg);
	remove(path);
	return 0;
}

static int test_try_config_reload_keeps_old_on_invalid_file(void)
{
	char path[128];
	config_t *cfg = NULL;

	ASSERT(test_runner_mkstemp_path("shellclaw_test_reload", path, sizeof(path)) == 0);
	cfg = load_minimal_config(path, "still-valid");
	ASSERT(cfg != NULL);
	bootstrap_set_config_path(path);
	bootstrap_set_cfg(cfg);
	{
		FILE *f = fopen(path, "w");
		ASSERT(f);
		fprintf(f, "[memory]\ndb_path = \"/tmp/db\"\n");
		fclose(f);
	}
	try_config_reload(&cfg);
	ASSERT(cfg != NULL);
	ASSERT(strcmp(config_agent_model(cfg), "still-valid") == 0);
	stale_free_all();
	config_free(cfg);
	remove(path);
	return 0;
}

static int test_try_config_reload_null_args_noop(void)
{
	config_t *cfg = NULL;
	try_config_reload(NULL);
	try_config_reload(&cfg);
	return 0;
}

int main(void)
{
	RUN(test_on_hup_sets_reload_flag());
	RUN(test_stale_enqueue_null_is_noop());
	RUN(test_stale_enqueue_preserves_old_config());
	RUN(test_stale_enqueue_preserves_independent_configs());
	RUN(test_try_config_reload_swaps_live_config());
	RUN(test_try_config_reload_keeps_old_on_invalid_file());
	RUN(test_try_config_reload_null_args_noop());
	printf("test_reload: all tests passed\n");
	return 0;
}
