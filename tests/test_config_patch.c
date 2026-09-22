/**
 * @file test_config_patch.c
 * @brief Unit tests for dashboard JSON config patching.
 */

#include "test_runner.h"
#include "src/core/config.h"
#include "src/core/config_patch.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int write_toml(const char *path, const char *toml)
{
	FILE *f = fopen(path, "w");
	if (!f)
		return -1;
	fputs(toml, f);
	fclose(f);
	return 0;
}

static int test_patch_model_and_temperature(void)
{
	char path[128];
	char *patched = NULL;
	size_t patched_len = 0;
	char errbuf[256];
	config_t *cfg = NULL;
	ASSERT(test_runner_mkstemp_path("shellclaw_test_config_patch", path, sizeof(path)) == 0);
	ASSERT(write_toml(path,
	                  "[agent]\nmodel = \"old-model\"\nmax_tokens = 1024\ntemperature = 0.2\n"
	                  "[gateway]\nhost = \"127.0.0.1\"\nport = 18789\n") == 0);
	ASSERT(config_patch_dashboard_json(
	           path, "{\"model\":\"new-model\",\"temperature\":0.9}", &patched, &patched_len, errbuf,
	           sizeof(errbuf)) == 0);
	ASSERT(patched != NULL);
	ASSERT(strstr(patched, "model = \"new-model\"") != NULL);
	ASSERT(strstr(patched, "temperature = 0.9") != NULL);
	ASSERT(strstr(patched, "max_tokens = 1024") != NULL);
	ASSERT(write_toml(path, patched) == 0);
	free(patched);
	ASSERT(config_load(path, &cfg, errbuf, sizeof(errbuf)) == 0);
	ASSERT(strcmp(config_agent_model(cfg), "new-model") == 0);
	ASSERT(config_agent_temperature(cfg) == 0.9);
	ASSERT(config_agent_max_tokens(cfg) == 1024);
	config_free(cfg);
	remove(path);
	return 0;
}

static int test_patch_inserts_missing_key(void)
{
	char path[128];
	char *patched = NULL;
	size_t patched_len = 0;
	char errbuf[256];
	config_t *cfg = NULL;
	ASSERT(test_runner_mkstemp_path("shellclaw_test_config_patch", path, sizeof(path)) == 0);
	ASSERT(write_toml(path, "[agent]\nmodel = \"old-model\"\n") == 0);
	ASSERT(config_patch_dashboard_json(path, "{\"max_tokens\":2048}", &patched, &patched_len,
	                                   errbuf, sizeof(errbuf)) == 0);
	ASSERT(patched != NULL);
	ASSERT(strstr(patched, "max_tokens = 2048") != NULL);
	ASSERT(strstr(patched, "model = \"old-model\"") != NULL);
	ASSERT(write_toml(path, patched) == 0);
	free(patched);
	ASSERT(config_load(path, &cfg, errbuf, sizeof(errbuf)) == 0);
	ASSERT(config_agent_max_tokens(cfg) == 2048);
	config_free(cfg);
	remove(path);
	return 0;
}

static int test_patch_rejects_invalid_json(void)
{
	char path[128];
	char *patched = NULL;
	size_t patched_len = 0;
	char errbuf[256];
	ASSERT(test_runner_mkstemp_path("shellclaw_test_config_patch", path, sizeof(path)) == 0);
	ASSERT(write_toml(path, "[agent]\nmodel = \"old-model\"\n") == 0);
	ASSERT(config_patch_dashboard_json(path, "not-json", &patched, &patched_len, errbuf,
	                                   sizeof(errbuf)) != 0);
	ASSERT(patched == NULL);
	remove(path);
	return 0;
}

static int test_patch_indented_key_and_commented_section(void)
{
	char path[128];
	char *patched = NULL;
	size_t patched_len = 0;
	char errbuf[256];
	config_t *cfg = NULL;
	ASSERT(test_runner_mkstemp_path("shellclaw_test_config_patch", path, sizeof(path)) == 0);
	ASSERT(write_toml(path,
	                  "[agent] # live\n  model = \"old\"\n"
	                  "[gateway]\n  host = \"127.0.0.1\"\n  port = 18789\n") == 0);
	ASSERT(config_patch_dashboard_json(path, "{\"model\":\"new\",\"gateway_port\":19000}",
	                                   &patched, &patched_len, errbuf, sizeof(errbuf)) == 0);
	ASSERT(patched != NULL);
	ASSERT(strstr(patched, "model = \"new\"") != NULL);
	ASSERT(strstr(patched, "model = \"old\"") == NULL);
	ASSERT(strstr(patched, "port = 19000") != NULL);
	ASSERT(strstr(patched, "[agent]") != NULL);
	/* One [agent] header, not a duplicate appended after a missed comment. */
	ASSERT(strstr(strstr(patched, "[agent]") + 1, "[agent]") == NULL);
	ASSERT(write_toml(path, patched) == 0);
	free(patched);
	ASSERT(config_load(path, &cfg, errbuf, sizeof(errbuf)) == 0);
	ASSERT(strcmp(config_agent_model(cfg), "new") == 0);
	ASSERT(config_gateway_port(cfg) == 19000);
	config_free(cfg);
	remove(path);
	return 0;
}

static int test_patch_escapes_quotes_and_newlines(void)
{
	char path[128];
	char *patched = NULL;
	size_t patched_len = 0;
	char errbuf[256];
	ASSERT(test_runner_mkstemp_path("shellclaw_test_config_patch", path, sizeof(path)) == 0);
	ASSERT(write_toml(path, "[agent]\nmodel = \"old\"\n") == 0);
	ASSERT(config_patch_dashboard_json(path, "{\"model\":\"a\\\"b\\nc\"}", &patched, &patched_len,
	                                   errbuf, sizeof(errbuf)) == 0);
	ASSERT(patched != NULL);
	ASSERT(strstr(patched, "model = \"a\\\"b\\nc\"") != NULL);
	free(patched);
	remove(path);
	return 0;
}

static int test_patch_rejects_wrong_json_types(void)
{
	char path[128];
	char *patched = NULL;
	size_t patched_len = 0;
	char errbuf[256];
	ASSERT(test_runner_mkstemp_path("shellclaw_test_config_patch", path, sizeof(path)) == 0);
	ASSERT(write_toml(path, "[agent]\nmodel = \"old\"\nmax_tokens = 1024\n") == 0);
	ASSERT(config_patch_dashboard_json(path, "{\"model\":123,\"max_tokens\":\"nope\"}",
	                                   &patched, &patched_len, errbuf, sizeof(errbuf)) != 0);
	ASSERT(patched == NULL);
	ASSERT(strstr(errbuf, "model") != NULL);
	memset(errbuf, 0, sizeof(errbuf));
	ASSERT(config_patch_dashboard_json(path, "{\"model\":null}", &patched, &patched_len, errbuf,
	                                   sizeof(errbuf)) != 0);
	ASSERT(patched == NULL);
	ASSERT(strstr(errbuf, "model") != NULL);
	remove(path);
	return 0;
}

static int test_patch_creates_missing_section(void)
{
	char path[128];
	char *patched = NULL;
	size_t patched_len = 0;
	char errbuf[256];
	config_t *cfg = NULL;
	ASSERT(test_runner_mkstemp_path("shellclaw_test_config_patch", path, sizeof(path)) == 0);
	ASSERT(write_toml(path, "[agent]\nmodel = \"old-model\"\n") == 0);
	ASSERT(config_patch_dashboard_json(path, "{\"gateway_host\":\"10.0.0.1\",\"gateway_port\":19000}",
	                                   &patched, &patched_len, errbuf, sizeof(errbuf)) == 0);
	ASSERT(patched != NULL);
	ASSERT(strstr(patched, "[gateway]") != NULL);
	ASSERT(strstr(patched, "host = \"10.0.0.1\"") != NULL);
	ASSERT(write_toml(path, patched) == 0);
	free(patched);
	ASSERT(config_load(path, &cfg, errbuf, sizeof(errbuf)) == 0);
	ASSERT(strcmp(config_gateway_host(cfg), "10.0.0.1") == 0);
	ASSERT(config_gateway_port(cfg) == 19000);
	config_free(cfg);
	remove(path);
	return 0;
}

int main(void)
{
	int failed = 0;
	if (test_patch_model_and_temperature() != 0) {
		fprintf(stderr, "test_patch_model_and_temperature failed\n");
		failed++;
	}
	if (test_patch_inserts_missing_key() != 0) {
		fprintf(stderr, "test_patch_inserts_missing_key failed\n");
		failed++;
	}
	if (test_patch_rejects_invalid_json() != 0) {
		fprintf(stderr, "test_patch_rejects_invalid_json failed\n");
		failed++;
	}
	if (test_patch_creates_missing_section() != 0) {
		fprintf(stderr, "test_patch_creates_missing_section failed\n");
		failed++;
	}
	if (test_patch_indented_key_and_commented_section() != 0) {
		fprintf(stderr, "test_patch_indented_key_and_commented_section failed\n");
		failed++;
	}
	if (test_patch_escapes_quotes_and_newlines() != 0) {
		fprintf(stderr, "test_patch_escapes_quotes_and_newlines failed\n");
		failed++;
	}
	if (test_patch_rejects_wrong_json_types() != 0) {
		fprintf(stderr, "test_patch_rejects_wrong_json_types failed\n");
		failed++;
	}
	if (failed == 0)
		printf("test_config_patch: all tests passed\n");
	return failed;
}
