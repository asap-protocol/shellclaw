/**
 * @file test_registry.c
 * @brief Registry smoke tests: hardware tools registered with valid JSON schemas.
 */
#define _POSIX_C_SOURCE 200809L

#include "tools/tool.h"
#include "core/config.h"
#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ASSERT(c) do { \
	if (!(c)) { \
		fprintf(stderr, "FAIL: %s:%d  %s\n", __FILE__, __LINE__, #c); \
		return 1; \
	} \
} while (0)
#define RUN(t) do { int _r = (t); if (_r) return _r; } while (0)

/* Legacy production cap that truncated hardware tools after six core slots. */
#define LEGACY_TOOL_TABLE_CAP 8

static int core_stub_exec(const char *args_json, char *result_buf, size_t max_len)
{
	(void)args_json;
	if (result_buf && max_len > 0)
		result_buf[0] = '\0';
	return 0;
}

/* Non-NULL stand-ins for production getters so tool_get_all occupies six core
 * slots. NULL stubs hid the 8-slot production cap in earlier tests. */
static const tool_t CORE_STUB_SHELL = {
	.name = "shell",
	.description = "core stub",
	.parameters_json = "{}",
	.execute = core_stub_exec,
};
static const tool_t CORE_STUB_WEB_SEARCH = {
	.name = "web_search",
	.description = "core stub",
	.parameters_json = "{}",
	.execute = core_stub_exec,
};
static const tool_t CORE_STUB_FILE = {
	.name = "file",
	.description = "core stub",
	.parameters_json = "{}",
	.execute = core_stub_exec,
};
static const tool_t CORE_STUB_CRON = {
	.name = "cron",
	.description = "core stub",
	.parameters_json = "{}",
	.execute = core_stub_exec,
};
static const tool_t CORE_STUB_CONTEXT = {
	.name = "context",
	.description = "core stub",
	.parameters_json = "{}",
	.execute = core_stub_exec,
};
static const tool_t CORE_STUB_ASAP_INVOKE = {
	.name = "asap_invoke",
	.description = "core stub",
	.parameters_json = "{}",
	.execute = core_stub_exec,
};

const tool_t *tool_shell_get(void) { return &CORE_STUB_SHELL; }
void tool_shell_set_config(const config_t *cfg) { (void)cfg; }
const tool_t *tool_web_search_get(void) { return &CORE_STUB_WEB_SEARCH; }
void tool_web_search_set_config(const config_t *cfg) { (void)cfg; }
const tool_t *tool_file_get(void) { return &CORE_STUB_FILE; }
void tool_file_set_config(const config_t *cfg) { (void)cfg; }
const tool_t *tool_cron_get(void) { return &CORE_STUB_CRON; }
const tool_t *tool_context_get(void) { return &CORE_STUB_CONTEXT; }
void tool_context_set_config(const config_t *cfg) { (void)cfg; }
const tool_t *tool_asap_invoke_get(void) { return &CORE_STUB_ASAP_INVOKE; }
void tool_asap_invoke_set_config(const config_t *cfg) { (void)cfg; }

static const char *const HW_TOOL_NAMES[] = {
	"gpio_read",
	"gpio_write",
	"gpio_mode",
	"i2c_read",
	"i2c_write",
	"i2c_scan",
	"camera_capture",
};

static const tool_t *find_tool_by_name(const tool_t **tools, size_t count, const char *name)
{
	size_t i;

	for (i = 0; i < count; i++) {
		if (tools[i] && tools[i]->name && strcmp(tools[i]->name, name) == 0)
			return tools[i];
	}
	return NULL;
}

static int assert_schema_valid(const tool_t *tool)
{
	cJSON *schema;

	ASSERT(tool->parameters_json != NULL);
	ASSERT(tool->parameters_json[0] != '\0');
	schema = cJSON_Parse(tool->parameters_json);
	ASSERT(schema != NULL);
	ASSERT(cJSON_IsObject(schema));
	cJSON_Delete(schema);
	return 0;
}

static int test_hardware_tools_registered(void)
{
	const char *path = "/tmp/shellclaw_test_registry.toml";
	FILE *f;
	config_t *cfg = NULL;
	char errbuf[256];
	const tool_t *tools[32];
	size_t i;
	size_t n;

	f = fopen(path, "w");
	ASSERT(f);
	fprintf(f, "[agent]\nmodel = \"test\"\n\n[hardware]\nenabled = true\n");
	fclose(f);

	ASSERT(config_load(path, &cfg, errbuf, sizeof(errbuf)) == 0);
	tool_set_config(cfg);

	n = tool_get_all(tools, sizeof(tools) / sizeof(tools[0]));
	ASSERT(n >= sizeof(HW_TOOL_NAMES) / sizeof(HW_TOOL_NAMES[0]));

	for (i = 0; i < sizeof(HW_TOOL_NAMES) / sizeof(HW_TOOL_NAMES[0]); i++) {
		const tool_t *t = find_tool_by_name(tools, n, HW_TOOL_NAMES[i]);

		ASSERT(t != NULL);
		ASSERT(t->description != NULL && t->description[0] != '\0');
		ASSERT(t->execute != NULL);
		RUN(assert_schema_valid(t));
	}

	config_free(cfg);
	remove(path);
	return 0;
}

static int test_hardware_tools_hidden_when_disabled(void)
{
	const char *path = "/tmp/shellclaw_test_registry_off.toml";
	FILE *f;
	config_t *cfg = NULL;
	char errbuf[256];
	const tool_t *tools[32];
	size_t n;

	f = fopen(path, "w");
	ASSERT(f);
	fprintf(f, "[agent]\nmodel = \"test\"\n\n[hardware]\nenabled = false\n");
	fclose(f);

	ASSERT(config_load(path, &cfg, errbuf, sizeof(errbuf)) == 0);
	tool_set_config(cfg);

	n = tool_get_all(tools, sizeof(tools) / sizeof(tools[0]));
	ASSERT(find_tool_by_name(tools, n, "gpio_read") == NULL);

	config_free(cfg);
	remove(path);
	return 0;
}

static int load_hw_enabled_tools(const char *path, const tool_t **tools, size_t max_count,
				  size_t *n_out, config_t **cfg_out)
{
	FILE *f;
	char errbuf[256];

	f = fopen(path, "w");
	ASSERT(f);
	fprintf(f, "[agent]\nmodel = \"test\"\n\n[hardware]\nenabled = true\n");
	fclose(f);
	ASSERT(config_load(path, cfg_out, errbuf, sizeof(errbuf)) == 0);
	tool_set_config(*cfg_out);
	*n_out = tool_get_all(tools, max_count);
	return 0;
}

static int assert_all_hardware_names(const tool_t **tools, size_t n)
{
	size_t i;

	for (i = 0; i < sizeof(HW_TOOL_NAMES) / sizeof(HW_TOOL_NAMES[0]); i++) {
		ASSERT(find_tool_by_name(tools, n, HW_TOOL_NAMES[i]) != NULL);
	}
	return 0;
}

static int test_hardware_tools_fit_production_cap(void)
{
	const char *path = "/tmp/shellclaw_test_registry_cap.toml";
	config_t *cfg = NULL;
	const tool_t *tools[SHELLCLAW_MAX_TOOLS];
	size_t n;

	RUN(load_hw_enabled_tools(path, tools, SHELLCLAW_MAX_TOOLS, &n, &cfg));
	ASSERT(n <= SHELLCLAW_MAX_TOOLS);
	ASSERT(n >= sizeof(HW_TOOL_NAMES) / sizeof(HW_TOOL_NAMES[0]));
	RUN(assert_all_hardware_names(tools, n));
	config_free(cfg);
	remove(path);
	return 0;
}

static int test_legacy_eight_slot_cap_drops_later_hardware(void)
{
	const char *path = "/tmp/shellclaw_test_registry_cap8.toml";
	config_t *cfg = NULL;
	const tool_t *tools[LEGACY_TOOL_TABLE_CAP];
	size_t n;

	RUN(load_hw_enabled_tools(path, tools, LEGACY_TOOL_TABLE_CAP, &n, &cfg));
	ASSERT(n == LEGACY_TOOL_TABLE_CAP);
	ASSERT(find_tool_by_name(tools, n, "gpio_read") != NULL);
	ASSERT(find_tool_by_name(tools, n, "gpio_write") != NULL);
	ASSERT(find_tool_by_name(tools, n, "gpio_mode") == NULL);
	ASSERT(find_tool_by_name(tools, n, "camera_capture") == NULL);
	config_free(cfg);
	remove(path);
	return 0;
}

int main(void)
{
	RUN(test_hardware_tools_registered());
	RUN(test_hardware_tools_hidden_when_disabled());
	RUN(test_hardware_tools_fit_production_cap());
	RUN(test_legacy_eight_slot_cap_drops_later_hardware());
	printf("test_registry: all tests passed\n");
	return 0;
}
