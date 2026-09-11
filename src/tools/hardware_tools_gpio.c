/**
 * @file hardware_tools_gpio.c
 * @brief GPIO tool executors (read, write, mode).
 */
#define _POSIX_C_SOURCE 200809L

#include "tools/hardware_tools_internal.h"
#include "hardware/hardware.h"
#include "tools/tool.h"
#include <stdio.h>
#include <string.h>

static const char GPIO_READ_PARAMS[] =
	"{\"type\":\"object\",\"properties\":{\"pin\":{\"type\":\"integer\","
	"\"description\":\"Physical 40-pin header pin number (1-40)\",\"minimum\":1,"
	"\"maximum\":40}},\"required\":[\"pin\"]}";

static const char GPIO_WRITE_PARAMS[] =
	"{\"type\":\"object\",\"properties\":{\"pin\":{\"type\":\"integer\","
	"\"description\":\"Physical header pin (1-40)\",\"minimum\":1,\"maximum\":40},"
	"\"value\":{\"type\":\"integer\",\"description\":\"0=LOW, 1=HIGH\",\"enum\":[0,1]}},"
	"\"required\":[\"pin\",\"value\"]}";

static const char GPIO_MODE_PARAMS[] =
	"{\"type\":\"object\",\"properties\":{\"pin\":{\"type\":\"integer\","
	"\"description\":\"Physical header pin (1-40)\",\"minimum\":1,\"maximum\":40},"
	"\"mode\":{\"type\":\"string\",\"enum\":[\"input\",\"output\"],"
	"\"description\":\"Pin direction\"}},\"required\":[\"pin\",\"mode\"]}";

static int gpio_read_exec(const char *args_json, char *result_buf, size_t max_len)
{
	cJSON *root = NULL;
	int pin;
	int value = 0;
	char errbuf[256];
	int rc;

	if (!hw_tools_enabled()) {
		snprintf(result_buf, max_len, "%s", HW_ERR_DISABLED);
		return -1;
	}
	if (hw_tools_parse_root(args_json, &root, result_buf, max_len) != 0)
		return -1;
	if (hw_tools_require_int(root, "pin", &pin, result_buf, max_len) != 0) {
		cJSON_Delete(root);
		return -1;
	}
	cJSON_Delete(root);
	if (hw_tools_validate_gpio_pin(pin, result_buf, max_len) != 0)
		return -1;
	if (!hw_tools_gpio_ready()) {
		snprintf(result_buf, max_len, "%s", HW_ERR_GPIO);
		return -1;
	}
#ifdef HAVE_LIBGPIOD
	rc = hardware_gpio_read(pin, &value, errbuf, sizeof(errbuf));
#else
	rc = -1;
	snprintf(errbuf, sizeof(errbuf), "GPIO not available");
#endif
	if (rc != 0) {
		hw_tools_json_error(result_buf, max_len, errbuf);
		return -1;
	}
	snprintf(result_buf, max_len, "{\"pin\":%d,\"value\":%d}", pin, value);
	return 0;
}

static int gpio_write_exec(const char *args_json, char *result_buf, size_t max_len)
{
	cJSON *root = NULL;
	int pin;
	int value;
	char errbuf[256];
	int rc;

	if (!hw_tools_enabled()) {
		snprintf(result_buf, max_len, "%s", HW_ERR_DISABLED);
		return -1;
	}
	if (hw_tools_parse_root(args_json, &root, result_buf, max_len) != 0)
		return -1;
	if (hw_tools_require_int(root, "pin", &pin, result_buf, max_len) != 0 ||
	    hw_tools_require_int(root, "value", &value, result_buf, max_len) != 0) {
		cJSON_Delete(root);
		return -1;
	}
	cJSON_Delete(root);
	if (hw_tools_validate_gpio_pin(pin, result_buf, max_len) != 0)
		return -1;
	if (value != 0 && value != 1) {
		snprintf(result_buf, max_len, "{\"error\":\"value must be 0 or 1 (got %d)\"}",
			 value);
		return -1;
	}
	if (!hw_tools_gpio_ready()) {
		snprintf(result_buf, max_len, "%s", HW_ERR_GPIO);
		return -1;
	}
#ifdef HAVE_LIBGPIOD
	rc = hardware_gpio_write(pin, value, errbuf, sizeof(errbuf));
#else
	rc = -1;
	snprintf(errbuf, sizeof(errbuf), "GPIO not available");
#endif
	if (rc != 0) {
		hw_tools_json_error(result_buf, max_len, errbuf);
		return -1;
	}
	snprintf(result_buf, max_len, "{\"pin\":%d,\"value\":%d}", pin, value ? 1 : 0);
	return 0;
}

static int gpio_mode_parse_args(const char *args_json, int *pin, char *mode_buf,
				size_t mode_buf_sz, char *result_buf, size_t max_len)
{
	cJSON *root = NULL;
	cJSON *mode_item;

	if (hw_tools_parse_root(args_json, &root, result_buf, max_len) != 0)
		return -1;
	if (hw_tools_require_int(root, "pin", pin, result_buf, max_len) != 0) {
		cJSON_Delete(root);
		return -1;
	}
	mode_item = cJSON_GetObjectItem(root, "mode");
	if (!mode_item || !cJSON_IsString(mode_item) || !mode_item->valuestring[0]) {
		cJSON_Delete(root);
		snprintf(result_buf, max_len, "{\"error\":\"missing or invalid mode\"}");
		return -1;
	}
	if (strcmp(mode_item->valuestring, "input") != 0 &&
	    strcmp(mode_item->valuestring, "output") != 0) {
		cJSON_Delete(root);
		snprintf(result_buf, max_len, "{\"error\":\"mode must be input or output\"}");
		return -1;
	}
	snprintf(mode_buf, mode_buf_sz, "%s", mode_item->valuestring);
	cJSON_Delete(root);
	return 0;
}

static int gpio_mode_exec(const char *args_json, char *result_buf, size_t max_len)
{
	int pin;
	char mode_buf[8];
	char errbuf[256];
	int rc;

	if (!hw_tools_enabled()) {
		snprintf(result_buf, max_len, "%s", HW_ERR_DISABLED);
		return -1;
	}
	if (gpio_mode_parse_args(args_json, &pin, mode_buf, sizeof(mode_buf), result_buf,
				  max_len) != 0)
		return -1;
	if (hw_tools_validate_gpio_pin(pin, result_buf, max_len) != 0)
		return -1;
	if (!hw_tools_gpio_ready()) {
		snprintf(result_buf, max_len, "%s", HW_ERR_GPIO);
		return -1;
	}
#ifdef HAVE_LIBGPIOD
	rc = hardware_gpio_mode(pin, mode_buf, errbuf, sizeof(errbuf));
#else
	rc = -1;
	snprintf(errbuf, sizeof(errbuf), "GPIO not available");
#endif
	if (rc != 0) {
		hw_tools_json_error(result_buf, max_len, errbuf);
		return -1;
	}
	snprintf(result_buf, max_len, "{\"pin\":%d,\"mode\":\"%s\"}", pin, mode_buf);
	return 0;
}

const tool_t HW_TOOLS_GPIO_READ = {
	.name = "gpio_read",
	.description = "Read the logic level (0=LOW, 1=HIGH) on a physical 40-pin header GPIO pin.",
	.parameters_json = GPIO_READ_PARAMS,
	.execute = gpio_read_exec,
};

const tool_t HW_TOOLS_GPIO_WRITE = {
	.name = "gpio_write",
	.description = "Drive a physical header GPIO pin HIGH (1) or LOW (0).",
	.parameters_json = GPIO_WRITE_PARAMS,
	.execute = gpio_write_exec,
};

const tool_t HW_TOOLS_GPIO_MODE = {
	.name = "gpio_mode",
	.description = "Configure a physical header pin as input or output before read/write.",
	.parameters_json = GPIO_MODE_PARAMS,
	.execute = gpio_mode_exec,
};
