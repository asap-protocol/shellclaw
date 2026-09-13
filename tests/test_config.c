/**
 * @file test_config.c
 * @brief Unit tests for config_load: TOML parsing, env overrides, validation.
 */

#include "test_runner.h"
#include "src/core/config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

static int test_load_valid_minimal(void)
{
	char path[128];
	FILE *f;
	ASSERT(test_runner_mkstemp_path("shellclaw_test_config", path, sizeof(path)) == 0);
	f = fopen(path, "w");
	ASSERT(f);
	fprintf(f, "[agent]\nmodel = \"claude-sonnet\"\nmax_tool_iterations = 10\n");
	fclose(f);
	config_t *cfg = NULL;
	char errbuf[256];
	int ret = config_load(path, &cfg, errbuf, sizeof(errbuf));
	ASSERT(ret == 0);
	ASSERT(cfg != NULL);
	ASSERT(config_agent_model(cfg) != NULL);
	ASSERT(strcmp(config_agent_model(cfg), "claude-sonnet") == 0);
	ASSERT(config_agent_max_tool_iterations(cfg) == 10);
	config_free(cfg);
	remove(path);
	return 0;
}

static int test_load_missing_agent_section(void)
{
	char path[128];
	FILE *f;
	ASSERT(test_runner_mkstemp_path("shellclaw_test_config", path, sizeof(path)) == 0);
	f = fopen(path, "w");
	ASSERT(f);
	fprintf(f, "[memory]\ndb_path = \"/tmp/db\"\n");
	fclose(f);
	config_t *cfg = NULL;
	char errbuf[256];
	int ret = config_load(path, &cfg, errbuf, sizeof(errbuf));
	ASSERT(ret != 0);
	ASSERT(cfg == NULL);
	ASSERT(strstr(errbuf, "agent") != NULL);
	remove(path);
	return 0;
}

static int test_load_missing_required_model(void)
{
	char path[128];
	FILE *f;
	ASSERT(test_runner_mkstemp_path("shellclaw_test_config", path, sizeof(path)) == 0);
	f = fopen(path, "w");
	ASSERT(f);
	fprintf(f, "[agent]\nmax_tokens = 2048\n");
	fclose(f);
	config_t *cfg = NULL;
	char errbuf[256];
	int ret = config_load(path, &cfg, errbuf, sizeof(errbuf));
	ASSERT(ret != 0);
	ASSERT(cfg == NULL);
	ASSERT(strstr(errbuf, "model") != NULL);
	remove(path);
	return 0;
}

static int test_env_override(void)
{
	char path[128];
	FILE *f;
	ASSERT(test_runner_mkstemp_path("shellclaw_test_config", path, sizeof(path)) == 0);
	f = fopen(path, "w");
	ASSERT(f);
	fprintf(f, "[agent]\nmodel = \"from-file\"\n");
	fclose(f);
	setenv("SHELLCLAW_AGENT_MODEL", "from-env", 1);
	config_t *cfg = NULL;
	char errbuf[256];
	int ret = config_load(path, &cfg, errbuf, sizeof(errbuf));
	unsetenv("SHELLCLAW_AGENT_MODEL");
	ASSERT(ret == 0);
	ASSERT(cfg != NULL);
	ASSERT(strcmp(config_agent_model(cfg), "from-env") == 0);
	config_free(cfg);
	remove(path);
	return 0;
}

static int test_defaults(void)
{
	char path[128];
	FILE *f;
	ASSERT(test_runner_mkstemp_path("shellclaw_test_config", path, sizeof(path)) == 0);
	f = fopen(path, "w");
	ASSERT(f);
	fprintf(f, "[agent]\nmodel = \"default-model\"\n");
	fclose(f);
	config_t *cfg = NULL;
	int ret = config_load(path, &cfg, NULL, 0);
	ASSERT(ret == 0);
	ASSERT(config_agent_max_tool_iterations(cfg) == 20);
	ASSERT(config_agent_max_context_messages(cfg) == 40);
	config_free(cfg);
	remove(path);
	return 0;
}

static int test_gateway_section(void)
{
	char path[128];
	FILE *f;
	ASSERT(test_runner_mkstemp_path("shellclaw_test_config", path, sizeof(path)) == 0);
	f = fopen(path, "w");
	ASSERT(f);
	fprintf(f, "[agent]\nmodel = \"test\"\n");
	fprintf(f, "[gateway]\nenabled = true\nhost = \"0.0.0.0\"\nport = 18789\nallow_bind_all = true\n");
	fclose(f);
	config_t *cfg = NULL;
	char errbuf[256];
	int ret = config_load(path, &cfg, errbuf, sizeof(errbuf));
	ASSERT(ret == 0);
	ASSERT(cfg != NULL);
	ASSERT(config_gateway_enabled(cfg) == 1);
	ASSERT(config_gateway_host(cfg) != NULL);
	ASSERT(strcmp(config_gateway_host(cfg), "0.0.0.0") == 0);
	ASSERT(config_gateway_port(cfg) == 18789);
	ASSERT(config_gateway_allow_bind_all(cfg) == 1);
	config_free(cfg);
	remove(path);
	return 0;
}

static int test_gateway_defaults(void)
{
	char path[128];
	FILE *f;
	ASSERT(test_runner_mkstemp_path("shellclaw_test_config", path, sizeof(path)) == 0);
	f = fopen(path, "w");
	ASSERT(f);
	fprintf(f, "[agent]\nmodel = \"test\"\n");
	fclose(f);
	config_t *cfg = NULL;
	int ret = config_load(path, &cfg, NULL, 0);
	ASSERT(ret == 0);
	ASSERT(config_gateway_enabled(cfg) == 0);
	ASSERT(config_gateway_host(cfg) != NULL);
	ASSERT(strcmp(config_gateway_host(cfg), "127.0.0.1") == 0);
	ASSERT(config_gateway_port(cfg) == 18789);
	ASSERT(config_gateway_allow_bind_all(cfg) == 0);
	config_free(cfg);
	remove(path);
	return 0;
}

static int test_heartbeat_section(void)
{
	char path[128];
	FILE *f;
	ASSERT(test_runner_mkstemp_path("shellclaw_test_config", path, sizeof(path)) == 0);
	f = fopen(path, "w");
	ASSERT(f);
	fprintf(f, "[agent]\nmodel = \"test\"\n");
	fprintf(f, "[heartbeat]\nenabled = true\ninterval_minutes = 5\ndefault_channel = \"log\"\n");
	fclose(f);
	config_t *cfg = NULL;
	char errbuf[256];
	int ret = config_load(path, &cfg, errbuf, sizeof(errbuf));
	ASSERT(ret == 0);
	ASSERT(cfg != NULL);
	ASSERT(config_heartbeat_enabled(cfg) == 1);
	ASSERT(config_heartbeat_interval_minutes(cfg) == 5);
	ASSERT(config_heartbeat_default_channel(cfg) != NULL);
	ASSERT(strcmp(config_heartbeat_default_channel(cfg), "log") == 0);
	config_free(cfg);
	remove(path);
	return 0;
}

static int test_heartbeat_defaults(void)
{
	char path[128];
	FILE *f;
	ASSERT(test_runner_mkstemp_path("shellclaw_test_config", path, sizeof(path)) == 0);
	f = fopen(path, "w");
	ASSERT(f);
	fprintf(f, "[agent]\nmodel = \"test\"\n");
	fclose(f);
	config_t *cfg = NULL;
	int ret = config_load(path, &cfg, NULL, 0);
	ASSERT(ret == 0);
	ASSERT(config_heartbeat_enabled(cfg) == 0);
	ASSERT(config_heartbeat_interval_minutes(cfg) == 30);
	ASSERT(config_heartbeat_default_channel(cfg) != NULL);
	ASSERT(strcmp(config_heartbeat_default_channel(cfg), "cli") == 0);
	config_free(cfg);
	remove(path);
	return 0;
}

static int test_web_search_brave_config(void)
{
	char path[128];
	FILE *f;
	ASSERT(test_runner_mkstemp_path("shellclaw_test_config", path, sizeof(path)) == 0);
	f = fopen(path, "w");
	ASSERT(f);
	fprintf(f, "[agent]\nmodel = \"test\"\n");
	fprintf(f, "[web_search]\nbrave_api_key_env = \"BRAVE_SEARCH_KEY\"\n");
	fclose(f);
	config_t *cfg = NULL;
	char errbuf[256];
	int ret = config_load(path, &cfg, errbuf, sizeof(errbuf));
	ASSERT(ret == 0);
	ASSERT(cfg != NULL);
	ASSERT(config_brave_api_key_env(cfg) != NULL);
	ASSERT(strcmp(config_brave_api_key_env(cfg), "BRAVE_SEARCH_KEY") == 0);
	config_free(cfg);
	remove(path);
	return 0;
}

static int test_web_search_brave_defaults(void)
{
	char path[128];
	FILE *f;
	ASSERT(test_runner_mkstemp_path("shellclaw_test_config", path, sizeof(path)) == 0);
	f = fopen(path, "w");
	ASSERT(f);
	fprintf(f, "[agent]\nmodel = \"test\"\n");
	fclose(f);
	config_t *cfg = NULL;
	int ret = config_load(path, &cfg, NULL, 0);
	ASSERT(ret == 0);
	ASSERT(config_brave_api_key_env(cfg) != NULL);
	ASSERT(strcmp(config_brave_api_key_env(cfg), "BRAVE_API_KEY") == 0);
	config_free(cfg);
	remove(path);
	return 0;
}

static int test_asap_registry_and_revocation_urls(void)
{
	char path[128];
	FILE *f;
	ASSERT(test_runner_mkstemp_path("shellclaw_test_config", path, sizeof(path)) == 0);
	f = fopen(path, "w");
	ASSERT(f);
	fprintf(f, "[agent]\nmodel = \"test\"\n");
	fprintf(f, "[asap]\nenabled = true\n");
	fprintf(f, "registry_url = \"https://registry.example/asap/registry.json\"\n");
	fprintf(f, "revocation_list_url = \"https://registry.example/asap/revoked_agents.json\"\n");
	fclose(f);
	unsetenv("SHELLCLAW_ASAP_REGISTRY_URL");
	unsetenv("SHELLCLAW_ASAP_REVOCATION_LIST_URL");
	config_t *cfg = NULL;
	char errbuf[256];
	int ret = config_load(path, &cfg, errbuf, sizeof(errbuf));
	ASSERT(ret == 0);
	ASSERT(cfg != NULL);
	ASSERT(config_asap_registry_url(cfg) != NULL);
	ASSERT(strcmp(config_asap_registry_url(cfg),
			"https://registry.example/asap/registry.json") == 0);
	ASSERT(config_asap_revocation_list_url(cfg) != NULL);
	ASSERT(strcmp(config_asap_revocation_list_url(cfg),
			"https://registry.example/asap/revoked_agents.json") == 0);
	config_free(cfg);
	remove(path);
	return 0;
}

static int test_asap_urls_env_override(void)
{
	char path[128];
	FILE *f;
	ASSERT(test_runner_mkstemp_path("shellclaw_test_config", path, sizeof(path)) == 0);
	f = fopen(path, "w");
	ASSERT(f);
	fprintf(f, "[agent]\nmodel = \"test\"\n");
	fprintf(f, "[asap]\n");
	fprintf(f, "registry_url = \"https://file.example/registry.json\"\n");
	fprintf(f, "revocation_list_url = \"https://file.example/revoked.json\"\n");
	fclose(f);
	setenv("SHELLCLAW_ASAP_REGISTRY_URL", "https://env.example/registry.json", 1);
	setenv("SHELLCLAW_ASAP_REVOCATION_LIST_URL", "https://env.example/revoked.json", 1);
	config_t *cfg = NULL;
	char errbuf[256];
	int ret = config_load(path, &cfg, errbuf, sizeof(errbuf));
	unsetenv("SHELLCLAW_ASAP_REGISTRY_URL");
	unsetenv("SHELLCLAW_ASAP_REVOCATION_LIST_URL");
	ASSERT(ret == 0);
	ASSERT(cfg != NULL);
	ASSERT(strcmp(config_asap_registry_url(cfg), "https://env.example/registry.json") == 0);
	ASSERT(strcmp(config_asap_revocation_list_url(cfg), "https://env.example/revoked.json") == 0);
	config_free(cfg);
	remove(path);
	return 0;
}

static int test_asap_trusted_senders(void)
{
	char path[128];
	FILE *f;
	ASSERT(test_runner_mkstemp_path("shellclaw_test_config", path, sizeof(path)) == 0);
	f = fopen(path, "w");
	ASSERT(f);
	fprintf(f, "[agent]\nmodel = \"test\"\n");
	fprintf(f, "[asap]\ntrusted_senders = [ \"urn:alpha\", \"urn:beta\" ]\n");
	fclose(f);
	config_t *cfg = NULL;
	char errbuf[256];
	int ret = config_load(path, &cfg, errbuf, sizeof(errbuf));
	ASSERT(ret == 0);
	ASSERT(cfg != NULL);
	ASSERT(config_asap_trusted_senders_count(cfg) == 2);
	ASSERT(strcmp(config_asap_trusted_sender(cfg, 0), "urn:alpha") == 0);
	ASSERT(strcmp(config_asap_trusted_sender(cfg, 1), "urn:beta") == 0);
	ASSERT(config_asap_trusted_sender(cfg, 2) == NULL);
	ASSERT(config_asap_trusted_sender(cfg, -1) == NULL);
	config_free(cfg);
	remove(path);
	return 0;
}

static int test_providers_fallback_defaults(void)
{
	char path[128];
	FILE *f;
	ASSERT(test_runner_mkstemp_path("shellclaw_test_config", path, sizeof(path)) == 0);
	f = fopen(path, "w");
	ASSERT(f);
	unsetenv("SHELLCLAW_FALLBACK_CHAIN");
	unsetenv("SHELLCLAW_LOCAL_ENDPOINT");
	unsetenv("SHELLCLAW_LOCAL_MODEL");
	fprintf(f, "[agent]\nmodel = \"shell\"\n");
	fclose(f);
	config_t *cfg = NULL;
	char errbuf[256];
	int ret = config_load(path, &cfg, errbuf, sizeof(errbuf));
	ASSERT(ret == 0);
	ASSERT(cfg != NULL);
	ASSERT(config_provider_fallback_chain_count(cfg) == 3);
	ASSERT(strcmp(config_provider_fallback_chain_entry(cfg, 0), "anthropic") == 0);
	ASSERT(strcmp(config_provider_fallback_chain_entry(cfg, 1), "openai") == 0);
	ASSERT(strcmp(config_provider_fallback_chain_entry(cfg, 2), "local") == 0);
	ASSERT(config_provider_fallback_chain_entry(cfg, 3) == NULL);
	ASSERT(strcmp(config_provider_local_endpoint(cfg), "http://127.0.0.1:8080/v1/chat/completions") == 0);
	ASSERT(strcmp(config_provider_local_model(cfg), "tinyllama-1.1b-q4") == 0);
	config_free(cfg);
	remove(path);
	return 0;
}

static int test_providers_fallback_toml_fragment(void)
{
	char path[128];
	FILE *f;
	ASSERT(test_runner_mkstemp_path("shellclaw_test_config", path, sizeof(path)) == 0);
	f = fopen(path, "w");
	ASSERT(f);
	unsetenv("SHELLCLAW_FALLBACK_CHAIN");
	unsetenv("SHELLCLAW_LOCAL_ENDPOINT");
	unsetenv("SHELLCLAW_LOCAL_MODEL");
	fprintf(f, "[agent]\nmodel = \"shell\"\n");
	fprintf(f, "[providers]\nfallback_chain = [ \"local\", \"openai\" ]\n\n");
	fprintf(f, "[providers.local]\nendpoint = \"http://127.7.7.7:9999/v1/chat/completions\"\nmodel = \"test-model-q4\"\n");
	fclose(f);
	config_t *cfg = NULL;
	char errbuf[256];
	int ret = config_load(path, &cfg, errbuf, sizeof(errbuf));
	ASSERT(ret == 0);
	ASSERT(cfg != NULL);
	ASSERT(config_provider_fallback_chain_count(cfg) == 2);
	ASSERT(strcmp(config_provider_fallback_chain_entry(cfg, 0), "local") == 0);
	ASSERT(strcmp(config_provider_fallback_chain_entry(cfg, 1), "openai") == 0);
	ASSERT(strcmp(config_provider_local_endpoint(cfg), "http://127.7.7.7:9999/v1/chat/completions") == 0);
	ASSERT(strcmp(config_provider_local_model(cfg), "test-model-q4") == 0);
	config_free(cfg);
	remove(path);
	return 0;
}

static int test_providers_fallback_env_override(void)
{
	char path[128];
	FILE *f;
	ASSERT(test_runner_mkstemp_path("shellclaw_test_config", path, sizeof(path)) == 0);
	f = fopen(path, "w");
	ASSERT(f);
	fprintf(f, "[agent]\nmodel = \"shell\"\n");
	fprintf(f, "[providers]\nfallback_chain = [ \"openai\" ]\n\n");
	fclose(f);
	setenv("SHELLCLAW_FALLBACK_CHAIN", "local, anthropic , openai", 1);
	setenv("SHELLCLAW_LOCAL_ENDPOINT", "http://env.example/v1/chat/completions", 1);
	setenv("SHELLCLAW_LOCAL_MODEL", "model-from-env", 1);
	config_t *cfg = NULL;
	char errbuf[256];
	int ret = config_load(path, &cfg, errbuf, sizeof(errbuf));
	unsetenv("SHELLCLAW_FALLBACK_CHAIN");
	unsetenv("SHELLCLAW_LOCAL_ENDPOINT");
	unsetenv("SHELLCLAW_LOCAL_MODEL");
	ASSERT(ret == 0);
	ASSERT(cfg != NULL);
	ASSERT(config_provider_fallback_chain_count(cfg) == 3);
	ASSERT(strcmp(config_provider_fallback_chain_entry(cfg, 0), "local") == 0);
	ASSERT(strcmp(config_provider_fallback_chain_entry(cfg, 1), "anthropic") == 0);
	ASSERT(strcmp(config_provider_fallback_chain_entry(cfg, 2), "openai") == 0);
	ASSERT(strcmp(config_provider_local_endpoint(cfg), "http://env.example/v1/chat/completions") == 0);
	ASSERT(strcmp(config_provider_local_model(cfg), "model-from-env") == 0);
	config_free(cfg);
	remove(path);
	return 0;
}

static int test_discord_minimal_block(void)
{
	char path[128];
	FILE *f;
	ASSERT(test_runner_mkstemp_path("shellclaw_test_config", path, sizeof(path)) == 0);
	f = fopen(path, "w");
	ASSERT(f);
	fprintf(f, "[agent]\nmodel = \"test\"\n");
	fprintf(f, "[channels.discord]\nenabled = true\n");
	fprintf(f, "allowed_user_ids = [ \"123456789012345678\", \"987654321098765432\" ]\n");
	fclose(f);
	unsetenv("DISCORD_BOT_TOKEN");
	config_t *cfg = NULL;
	char errbuf[256];
	int ret = config_load(path, &cfg, errbuf, sizeof(errbuf));
	ASSERT(ret == 0);
	ASSERT(cfg != NULL);
	ASSERT(config_discord_enabled(cfg) == 1);
	ASSERT(strcmp(config_discord_token_env(cfg), "DISCORD_BOT_TOKEN") == 0);
	ASSERT(config_discord_allowed_user_ids_count(cfg) == 2);
	ASSERT(strcmp(config_discord_allowed_user_id(cfg, 0), "123456789012345678") == 0);
	ASSERT(strcmp(config_discord_allowed_user_id(cfg, 1), "987654321098765432") == 0);
	ASSERT(config_discord_allowed_user_id(cfg, 2) == NULL);
	config_free(cfg);
	remove(path);
	return 0;
}

static int test_discord_token_env_override(void)
{
	char path[128];
	FILE *f;
	ASSERT(test_runner_mkstemp_path("shellclaw_test_config", path, sizeof(path)) == 0);
	f = fopen(path, "w");
	ASSERT(f);
	fprintf(f, "[agent]\nmodel = \"test\"\n");
	fprintf(f, "[channels.discord]\nenabled = false\n");
	fprintf(f, "token_env = \"MY_CUSTOM_DISCORD_ENV\"\n");
	fprintf(f, "allowed_user_ids = []\n");
	fclose(f);
	config_t *cfg = NULL;
	char errbuf[256];
	int ret = config_load(path, &cfg, errbuf, sizeof(errbuf));
	ASSERT(ret == 0);
	ASSERT(cfg != NULL);
	ASSERT(config_discord_enabled(cfg) == 0);
	ASSERT(strcmp(config_discord_token_env(cfg), "MY_CUSTOM_DISCORD_ENV") == 0);
	ASSERT(config_discord_allowed_user_ids_count(cfg) == 0);
	config_free(cfg);
	remove(path);
	return 0;
}

static int test_discord_defaults_when_absent(void)
{
	char path[128];
	FILE *f;
	ASSERT(test_runner_mkstemp_path("shellclaw_test_config", path, sizeof(path)) == 0);
	f = fopen(path, "w");
	ASSERT(f);
	fprintf(f, "[agent]\nmodel = \"test\"\n");
	fclose(f);
	config_t *cfg = NULL;
	int ret = config_load(path, &cfg, NULL, 0);
	ASSERT(ret == 0);
	ASSERT(config_discord_enabled(cfg) == 0);
	ASSERT(strcmp(config_discord_token_env(cfg), "DISCORD_BOT_TOKEN") == 0);
	ASSERT(config_discord_allowed_user_ids_count(cfg) == 0);
	config_free(cfg);
	remove(path);
	return 0;
}

static int test_telegram_section(void)
{
	char path[128];
	FILE *f;
	ASSERT(test_runner_mkstemp_path("shellclaw_test_config", path, sizeof(path)) == 0);
	f = fopen(path, "w");
	ASSERT(f);
	fprintf(f, "[agent]\nmodel = \"test\"\n");
	fprintf(f, "[channels.telegram]\nenabled = true\n");
	fprintf(f, "token_env = \"MY_TG_TOKEN\"\n");
	fprintf(f, "allowed_users = [ \"111\", \"222\" ]\n");
	fclose(f);
	config_t *cfg = NULL;
	char errbuf[256];
	int ret = config_load(path, &cfg, errbuf, sizeof(errbuf));
	ASSERT(ret == 0);
	ASSERT(config_telegram_enabled(cfg) == 1);
	ASSERT(strcmp(config_telegram_token_env(cfg), "MY_TG_TOKEN") == 0);
	ASSERT(config_telegram_allowed_users_count(cfg) == 2);
	ASSERT(strcmp(config_telegram_allowed_user(cfg, 0), "111") == 0);
	ASSERT(strcmp(config_telegram_allowed_user(cfg, 1), "222") == 0);
	config_free(cfg);
	remove(path);
	return 0;
}

static int test_sandbox_section(void)
{
	char path[128];
	FILE *f;
	ASSERT(test_runner_mkstemp_path("shellclaw_test_config", path, sizeof(path)) == 0);
	f = fopen(path, "w");
	ASSERT(f);
	fprintf(f, "[agent]\nmodel = \"test\"\n");
	fprintf(f, "[sandbox]\nenabled = true\nmemory_max_bytes = 134217728\ncpu_max = \"0.5\"\n");
	fprintf(f, "cgroup_base = \"/sys/fs/cgroup/shellclaw\"\n");
	fclose(f);
	config_t *cfg = NULL;
	char errbuf[256];
	int ret = config_load(path, &cfg, errbuf, sizeof(errbuf));
	ASSERT(ret == 0);
	ASSERT(config_sandbox_enabled(cfg) == 1);
	ASSERT(config_sandbox_memory_max_bytes(cfg) == 134217728U);
	ASSERT(strcmp(config_sandbox_cpu_max(cfg), "0.5") == 0);
	ASSERT(strcmp(config_sandbox_cgroup_base(cfg), "/sys/fs/cgroup/shellclaw") == 0);
	config_free(cfg);
	remove(path);
	return 0;
}

static int test_agent_geo_accessors(void)
{
	char path[128];
	FILE *f;
	ASSERT(test_runner_mkstemp_path("shellclaw_test_config", path, sizeof(path)) == 0);
	f = fopen(path, "w");
	ASSERT(f);
	fprintf(f, "[agent]\nmodel = \"test\"\n");
	fprintf(f, "latitude = 48.8566\nlongitude = 2.3522\ncountry_code = \"FR\"\n");
	fclose(f);
	config_t *cfg = NULL;
	char errbuf[256];
	int ret = config_load(path, &cfg, errbuf, sizeof(errbuf));
	ASSERT(ret == 0);
	ASSERT(config_agent_has_latitude(cfg) == 1);
	ASSERT(config_agent_has_longitude(cfg) == 1);
	ASSERT(config_agent_latitude(cfg) > 48.85 && config_agent_latitude(cfg) < 48.87);
	ASSERT(config_agent_longitude(cfg) > 2.35 && config_agent_longitude(cfg) < 2.36);
	ASSERT(strcmp(config_agent_country_code(cfg), "FR") == 0);
	config_free(cfg);
	remove(path);
	return 0;
}

static int test_hardware_defaults(void)
{
	char path[128];
	FILE *f;
	ASSERT(test_runner_mkstemp_path("shellclaw_test_config", path, sizeof(path)) == 0);
	f = fopen(path, "w");
	ASSERT(f);
	unsetenv("SHELLCLAW_BOARD");
	unsetenv("SHELLCLAW_I2C_BUS");
	unsetenv("SHELLCLAW_CAMERA_TYPE");
	fprintf(f, "[agent]\nmodel = \"test\"\n");
	fclose(f);
	config_t *cfg = NULL;
	int ret = config_load(path, &cfg, NULL, 0);
	ASSERT(ret == 0);
	ASSERT(config_hardware_enabled(cfg) == 1);
	ASSERT(config_hardware_board(cfg) == NULL);
	ASSERT(config_hardware_has_i2c_bus(cfg) == 0);
	ASSERT(config_hardware_i2c_bus(cfg) == 0);
	ASSERT(strcmp(config_hardware_camera_type(cfg), "auto") == 0);
	ASSERT(strcmp(config_hardware_camera_resolution(cfg), "640x480") == 0);
	ASSERT(config_hardware_camera_quality(cfg) == 75);
	ASSERT(config_hardware_has_gpio_test_pin(cfg) == 0);
	ASSERT(config_hardware_gpio_test_pin(cfg) == 0);
	config_free(cfg);
	remove(path);
	return 0;
}

static int test_hardware_section(void)
{
	char path[128];
	FILE *f;
	ASSERT(test_runner_mkstemp_path("shellclaw_test_config", path, sizeof(path)) == 0);
	f = fopen(path, "w");
	ASSERT(f);
	unsetenv("SHELLCLAW_BOARD");
	unsetenv("SHELLCLAW_I2C_BUS");
	unsetenv("SHELLCLAW_CAMERA_TYPE");
	fprintf(f, "[agent]\nmodel = \"test\"\n");
	fprintf(f, "[hardware]\nenabled = false\nboard = \"jetson\"\n");
	fprintf(f, "i2c_bus = 7\ncamera_type = \"csi\"\n");
	fprintf(f, "camera_resolution = \"1280x720\"\ncamera_quality = 90\ngpio_test_pin = 13\n");
	fclose(f);
	config_t *cfg = NULL;
	char errbuf[256];
	int ret = config_load(path, &cfg, errbuf, sizeof(errbuf));
	ASSERT(ret == 0);
	ASSERT(config_hardware_enabled(cfg) == 0);
	ASSERT(config_hardware_board(cfg) != NULL);
	ASSERT(strcmp(config_hardware_board(cfg), "jetson") == 0);
	ASSERT(config_hardware_has_i2c_bus(cfg) == 1);
	ASSERT(config_hardware_i2c_bus(cfg) == 7);
	ASSERT(strcmp(config_hardware_camera_type(cfg), "csi") == 0);
	ASSERT(strcmp(config_hardware_camera_resolution(cfg), "1280x720") == 0);
	ASSERT(config_hardware_camera_quality(cfg) == 90);
	ASSERT(config_hardware_has_gpio_test_pin(cfg) == 1);
	ASSERT(config_hardware_gpio_test_pin(cfg) == 13);
	config_free(cfg);
	remove(path);
	return 0;
}

static int test_hardware_env_override(void)
{
	char path[128];
	FILE *f;
	ASSERT(test_runner_mkstemp_path("shellclaw_test_config", path, sizeof(path)) == 0);
	f = fopen(path, "w");
	ASSERT(f);
	fprintf(f, "[agent]\nmodel = \"test\"\n");
	fprintf(f, "[hardware]\nboard = \"rpi\"\ni2c_bus = 1\ncamera_type = \"usb\"\n");
	fclose(f);
	setenv("SHELLCLAW_BOARD", "stub", 1);
	setenv("SHELLCLAW_I2C_BUS", "7", 1);
	setenv("SHELLCLAW_CAMERA_TYPE", "auto", 1);
	config_t *cfg = NULL;
	char errbuf[256];
	int ret = config_load(path, &cfg, errbuf, sizeof(errbuf));
	unsetenv("SHELLCLAW_BOARD");
	unsetenv("SHELLCLAW_I2C_BUS");
	unsetenv("SHELLCLAW_CAMERA_TYPE");
	ASSERT(ret == 0);
	ASSERT(strcmp(config_hardware_board(cfg), "stub") == 0);
	ASSERT(config_hardware_has_i2c_bus(cfg) == 1);
	ASSERT(config_hardware_i2c_bus(cfg) == 7);
	ASSERT(strcmp(config_hardware_camera_type(cfg), "auto") == 0);
	config_free(cfg);
	remove(path);
	return 0;
}

static int test_reload_sees_disk_change(void)
{
	char path[128];
	FILE *f;
	ASSERT(test_runner_mkstemp_path("shellclaw_test_config", path, sizeof(path)) == 0);
	f = fopen(path, "w");
	config_t *first = NULL;
	config_t *second = NULL;
	char errbuf[256];
	ASSERT(f);
	fprintf(f, "[agent]\nmodel = \"reload-a\"\nmax_tokens = 1111\n");
	fclose(f);
	ASSERT(config_load(path, &first, errbuf, sizeof(errbuf)) == 0);
	ASSERT(config_agent_max_tokens(first) == 1111);
	f = fopen(path, "w");
	ASSERT(f);
	fprintf(f, "[agent]\nmodel = \"reload-a\"\nmax_tokens = 2222\n");
	fclose(f);
	ASSERT(config_load(path, &second, errbuf, sizeof(errbuf)) == 0);
	ASSERT(config_agent_max_tokens(second) == 2222);
	ASSERT(config_agent_max_tokens(first) == 1111);
	config_free(second);
	config_free(first);
	remove(path);
	return 0;
}

static int test_reload_stale_config_independent(void)
{
	char path[128];
	FILE *f;
	ASSERT(test_runner_mkstemp_path("shellclaw_test_config", path, sizeof(path)) == 0);
	f = fopen(path, "w");
	config_t *stale = NULL;
	config_t *live = NULL;
	char errbuf[256];
	ASSERT(f);
	fprintf(f, "[agent]\nmodel = \"stale-model\"\nmax_tokens = 100\n");
	fclose(f);
	ASSERT(config_load(path, &stale, errbuf, sizeof(errbuf)) == 0);
	ASSERT(strcmp(config_agent_model(stale), "stale-model") == 0);
	f = fopen(path, "w");
	ASSERT(f);
	fprintf(f, "[agent]\nmodel = \"live-model\"\nmax_tokens = 200\n");
	fclose(f);
	ASSERT(config_load(path, &live, errbuf, sizeof(errbuf)) == 0);
	ASSERT(strcmp(config_agent_model(live), "live-model") == 0);
	ASSERT(strcmp(config_agent_model(stale), "stale-model") == 0);
	ASSERT(config_agent_max_tokens(stale) == 100);
	ASSERT(config_agent_max_tokens(live) == 200);
	config_free(live);
	ASSERT(strcmp(config_agent_model(stale), "stale-model") == 0);
	config_free(stale);
	remove(path);
	return 0;
}

int main(void)
{
	RUN(test_load_valid_minimal());
	RUN(test_load_missing_agent_section());
	RUN(test_load_missing_required_model());
	RUN(test_env_override());
	RUN(test_defaults());
	RUN(test_gateway_section());
	RUN(test_gateway_defaults());
	RUN(test_heartbeat_section());
	RUN(test_heartbeat_defaults());
	RUN(test_web_search_brave_config());
	RUN(test_web_search_brave_defaults());
	RUN(test_asap_registry_and_revocation_urls());
	RUN(test_asap_urls_env_override());
	RUN(test_asap_trusted_senders());
	RUN(test_providers_fallback_defaults());
	RUN(test_providers_fallback_toml_fragment());
	RUN(test_providers_fallback_env_override());
	RUN(test_discord_minimal_block());
	RUN(test_discord_token_env_override());
	RUN(test_discord_defaults_when_absent());
	RUN(test_telegram_section());
	RUN(test_sandbox_section());
	RUN(test_agent_geo_accessors());
	RUN(test_hardware_defaults());
	RUN(test_hardware_section());
	RUN(test_hardware_env_override());
	RUN(test_reload_sees_disk_change());
	RUN(test_reload_stale_config_independent());
	printf("test_config: all tests passed\n");
	return 0;
}
