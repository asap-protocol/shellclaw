/**
 * @file hardware_tools_helpers.c
 * @brief JSON parsing and validation helpers for hardware tools.
 */
#define _POSIX_C_SOURCE 200809L

#include "tools/hardware_tools_internal.h"
#include "hardware/hardware.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* TODO-ref(phase5-slice05-H1): g_hw_cfg is a module-local setter global, one
 * instance of the project-wide tool_X_set_config convention (shell, file,
 * web_search, asap_invoke, context, hardware). The whole-convention refactor
 * (pass const config_t *cfg / tool_context_t through tool_t.execute for ALL
 * tools) is scheduled for v1.0.1; it exceeds the 10-file/300-line PR threshold
 * and touches tool.h/agent.h vtables + ~30 test sites. See AGENTS.md "Known
 * debt". */
const config_t *g_hw_cfg;

int hw_tools_enabled(void)
{
	return g_hw_cfg != NULL && config_hardware_enabled(g_hw_cfg);
}

int hw_tools_parse_root(const char *args_json, cJSON **root_out, char *result_buf,
			size_t max_len)
{
	cJSON *root;

	if (!args_json || !root_out || !result_buf || max_len == 0)
		return -1;
	root = cJSON_Parse(args_json);
	if (!root || !cJSON_IsObject(root)) {
		if (root)
			cJSON_Delete(root);
		snprintf(result_buf, max_len, "{\"error\":\"invalid JSON\"}");
		return -1;
	}
	*root_out = root;
	return 0;
}

int hw_tools_require_int(cJSON *root, const char *key, int *out, char *result_buf,
			 size_t max_len)
{
	cJSON *item = cJSON_GetObjectItem(root, key);

	if (!item || !cJSON_IsNumber(item)) {
		snprintf(result_buf, max_len, "{\"error\":\"missing or invalid %s\"}", key);
		return -1;
	}
	*out = item->valueint;
	return 0;
}

void hw_tools_json_error(char *result_buf, size_t max_len, const char *msg)
{
	cJSON *obj = cJSON_CreateObject();

	if (!obj) {
		snprintf(result_buf, max_len, "{\"error\":\"internal error\"}");
		return;
	}
	cJSON_AddStringToObject(obj, "error", msg ? msg : "unknown error");
	{
		char *printed = cJSON_PrintUnformatted(obj);
		if (printed) {
			snprintf(result_buf, max_len, "%s", printed);
			free(printed);
		} else {
			snprintf(result_buf, max_len, "{\"error\":\"internal error\"}");
		}
	}
	cJSON_Delete(obj);
}

int hw_tools_parse_byte_array(cJSON *root, const char *key, uint8_t *out, size_t max_len,
			      size_t *len_out, char *result_buf, size_t result_max)
{
	cJSON *arr = cJSON_GetObjectItem(root, key);
	cJSON *item;
	size_t count = 0;

	if (!arr || !cJSON_IsArray(arr)) {
		snprintf(result_buf, result_max, "{\"error\":\"missing or invalid %s\"}", key);
		return -1;
	}
	cJSON_ArrayForEach(item, arr) {
		if (!cJSON_IsNumber(item) || item->valueint < 0 || item->valueint > 255) {
			snprintf(result_buf, result_max, "{\"error\":\"invalid byte in %s\"}", key);
			return -1;
		}
		if (count >= max_len) {
			snprintf(result_buf, result_max, "{\"error\":\"%s too long\"}", key);
			return -1;
		}
		out[count++] = (uint8_t)item->valueint;
	}
	if (count == 0) {
		snprintf(result_buf, result_max, "{\"error\":\"%s must not be empty\"}", key);
		return -1;
	}
	*len_out = count;
	return 0;
}

int hw_tools_validate_gpio_pin(int pin, char *result_buf, size_t max_len)
{
	if (pin < 1 || pin > 40) {
		snprintf(result_buf, max_len, "{\"error\":\"pin must be 1-40\"}");
		return -1;
	}
	return 0;
}

int hw_tools_validate_i2c_addr(int addr, char *result_buf, size_t max_len)
{
	if (addr < 0x03 || addr > 0x77) {
		snprintf(result_buf, max_len, "{\"error\":\"addr must be 0x03-0x77\"}");
		return -1;
	}
	return 0;
}

int hw_tools_resolve_i2c_bus(cJSON *root, int *bus_out, char *result_buf, size_t max_len)
{
	cJSON *item = cJSON_GetObjectItem(root, "bus");

	if (item != NULL) {
		if (!cJSON_IsNumber(item)) {
			snprintf(result_buf, max_len, "{\"error\":\"missing or invalid bus\"}");
			return -1;
		}
		if (item->valueint < HW_I2C_BUS_MIN || item->valueint > HW_I2C_BUS_MAX) {
			snprintf(result_buf, max_len, "{\"error\":\"bus must be 0-255\"}");
			return -1;
		}
		*bus_out = item->valueint;
		return 0;
	}
	*bus_out = hardware_resolve_i2c_bus(g_hw_cfg);
	return 0;
}

int hw_tools_gpio_ready(void)
{
#ifdef HAVE_LIBGPIOD
	return hardware_active_gpio_backend() == HARDWARE_GPIO_BACKEND_LIBGPIOD &&
	       hardware_libgpiod_is_available();
#else
	return 0;
#endif
}
