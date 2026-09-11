/**
 * @file hardware_tools_i2c.c
 * @brief I2C tool executors (read, write, scan).
 */
#define _POSIX_C_SOURCE 200809L

#include "tools/hardware_tools_internal.h"
#include "hardware/hardware.h"
#include "tools/tool.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char I2C_READ_PARAMS[] =
	"{\"type\":\"object\",\"properties\":{\"bus\":{\"type\":\"integer\","
	"\"description\":\"I2C bus number (default from config or board)\","
	"\"minimum\":0,\"maximum\":255},"
	"\"addr\":{\"type\":\"integer\",\"description\":\"7-bit I2C address\","
	"\"minimum\":3,\"maximum\":119},\"reg\":{\"type\":\"integer\","
	"\"description\":\"Register address\",\"minimum\":0,\"maximum\":255},"
	"\"len\":{\"type\":\"integer\",\"description\":\"Bytes to read\","
	"\"minimum\":1,\"maximum\":256}},\"required\":[\"addr\",\"reg\",\"len\"]}";

static const char I2C_WRITE_PARAMS[] =
	"{\"type\":\"object\",\"properties\":{\"bus\":{\"type\":\"integer\","
	"\"description\":\"I2C bus number (default from config or board)\","
	"\"minimum\":0,\"maximum\":255},"
	"\"addr\":{\"type\":\"integer\",\"minimum\":3,\"maximum\":119},"
	"\"reg\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":255},\"data\":{"
	"\"type\":\"array\",\"items\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":255},"
	"\"description\":\"Payload bytes to write after the register byte\"}},"
	"\"required\":[\"addr\",\"reg\",\"data\"]}";

static const char I2C_SCAN_PARAMS[] =
	"{\"type\":\"object\",\"properties\":{\"bus\":{\"type\":\"integer\","
	"\"description\":\"I2C bus number to probe (default from config or board)\","
	"\"minimum\":0,\"maximum\":255}}}";

static int i2c_read_exec(const char *args_json, char *result_buf, size_t max_len)
{
	cJSON *root = NULL;
	int bus;
	int addr;
	int reg;
	int len;
	uint8_t *buf = NULL;
	char errbuf[256];
	cJSON *arr;
	size_t i;
	int rc;

	if (!hw_tools_enabled()) {
		snprintf(result_buf, max_len, "%s", HW_ERR_DISABLED);
		return -1;
	}
	if (!hardware_active_i2c_backend() || !hardware_i2c_is_available()) {
		snprintf(result_buf, max_len, "%s", HW_ERR_I2C);
		return -1;
	}
	if (hw_tools_parse_root(args_json, &root, result_buf, max_len) != 0)
		return -1;
	if (hw_tools_resolve_i2c_bus(root, &bus, result_buf, max_len) != 0 ||
	    hw_tools_require_int(root, "addr", &addr, result_buf, max_len) != 0 ||
	    hw_tools_require_int(root, "reg", &reg, result_buf, max_len) != 0 ||
	    hw_tools_require_int(root, "len", &len, result_buf, max_len) != 0) {
		cJSON_Delete(root);
		return -1;
	}
	cJSON_Delete(root);
	if (hw_tools_validate_i2c_addr(addr, result_buf, max_len) != 0)
		return -1;
	if (len <= 0 || len > 256) {
		snprintf(result_buf, max_len, "{\"error\":\"len must be 1-256\"}");
		return -1;
	}
	buf = (uint8_t *)malloc((size_t)len);
	if (!buf) {
		snprintf(result_buf, max_len, "{\"error\":\"out of memory\"}");
		return -1;
	}
	rc = hardware_i2c_read(bus, (uint8_t)addr, (uint8_t)reg, (size_t)len, buf, errbuf,
			       sizeof(errbuf));
	if (rc != 0) {
		free(buf);
		hw_tools_json_error(result_buf, max_len, errbuf);
		return -1;
	}
	arr = cJSON_CreateArray();
	if (!arr) {
		free(buf);
		snprintf(result_buf, max_len, "{\"error\":\"out of memory\"}");
		return -1;
	}
	for (i = 0; i < (size_t)len; i++)
		cJSON_AddItemToArray(arr, cJSON_CreateNumber((double)buf[i]));
	free(buf);
	{
		cJSON *obj = cJSON_CreateObject();
		char *printed;
		if (!obj) {
			cJSON_Delete(arr);
			snprintf(result_buf, max_len, "{\"error\":\"out of memory\"}");
			return -1;
		}
		cJSON_AddItemToObject(obj, "data", arr);
		printed = cJSON_PrintUnformatted(obj);
		if (printed) {
			snprintf(result_buf, max_len, "%s", printed);
			free(printed);
		} else {
			snprintf(result_buf, max_len, "{\"error\":\"internal error\"}");
		}
		cJSON_Delete(obj);
	}
	return 0;
}

static int i2c_write_exec(const char *args_json, char *result_buf, size_t max_len)
{
	cJSON *root = NULL;
	int bus;
	int addr;
	int reg;
	uint8_t data[256];
	size_t data_len = 0;
	char errbuf[256];
	int rc;

	if (!hw_tools_enabled()) {
		snprintf(result_buf, max_len, "%s", HW_ERR_DISABLED);
		return -1;
	}
	if (!hardware_active_i2c_backend() || !hardware_i2c_is_available()) {
		snprintf(result_buf, max_len, "%s", HW_ERR_I2C);
		return -1;
	}
	if (hw_tools_parse_root(args_json, &root, result_buf, max_len) != 0)
		return -1;
	if (hw_tools_resolve_i2c_bus(root, &bus, result_buf, max_len) != 0 ||
	    hw_tools_require_int(root, "addr", &addr, result_buf, max_len) != 0 ||
	    hw_tools_require_int(root, "reg", &reg, result_buf, max_len) != 0 ||
	    hw_tools_parse_byte_array(root, "data", data, sizeof(data), &data_len, result_buf,
				      max_len) != 0) {
		cJSON_Delete(root);
		return -1;
	}
	cJSON_Delete(root);
	if (hw_tools_validate_i2c_addr(addr, result_buf, max_len) != 0)
		return -1;
	rc = hardware_i2c_write(bus, (uint8_t)addr, (uint8_t)reg, data, data_len, errbuf,
				  sizeof(errbuf));
	if (rc != 0) {
		hw_tools_json_error(result_buf, max_len, errbuf);
		return -1;
	}
	snprintf(result_buf, max_len, "{\"bus\":%d,\"addr\":%d,\"reg\":%d,\"bytes_written\":%zu}",
		 bus, addr, reg, data_len);
	return 0;
}

static int i2c_scan_exec(const char *args_json, char *result_buf, size_t max_len)
{
	cJSON *root = NULL;
	int bus;
	uint8_t addrs[128];
	int count = 0;
	char errbuf[256];
	cJSON *arr;
	int i;
	int rc;

	if (!hw_tools_enabled()) {
		snprintf(result_buf, max_len, "%s", HW_ERR_DISABLED);
		return -1;
	}
	if (!hardware_active_i2c_backend() || !hardware_i2c_is_available()) {
		snprintf(result_buf, max_len, "%s", HW_ERR_I2C);
		return -1;
	}
	if (!args_json || args_json[0] == '\0')
		root = cJSON_CreateObject();
	else if (hw_tools_parse_root(args_json, &root, result_buf, max_len) != 0)
		return -1;
	if (hw_tools_resolve_i2c_bus(root, &bus, result_buf, max_len) != 0) {
		cJSON_Delete(root);
		return -1;
	}
	cJSON_Delete(root);
	rc = hardware_i2c_scan(bus, addrs, (int)sizeof(addrs), &count, errbuf, sizeof(errbuf));
	if (rc != 0) {
		hw_tools_json_error(result_buf, max_len, errbuf);
		return -1;
	}
	arr = cJSON_CreateArray();
	if (!arr) {
		snprintf(result_buf, max_len, "{\"error\":\"out of memory\"}");
		return -1;
	}
	for (i = 0; i < count; i++)
		cJSON_AddItemToArray(arr, cJSON_CreateNumber((double)addrs[i]));
	{
		cJSON *obj = cJSON_CreateObject();
		char *printed;
		if (!obj) {
			cJSON_Delete(arr);
			snprintf(result_buf, max_len, "{\"error\":\"out of memory\"}");
			return -1;
		}
		cJSON_AddItemToObject(obj, "addresses", arr);
		printed = cJSON_PrintUnformatted(obj);
		if (printed) {
			snprintf(result_buf, max_len, "%s", printed);
			free(printed);
		} else {
			snprintf(result_buf, max_len, "{\"error\":\"internal error\"}");
		}
		cJSON_Delete(obj);
	}
	return 0;
}

const tool_t HW_TOOLS_I2C_READ = {
	.name = "i2c_read",
	.description = "Read bytes from an I2C device register on the given bus.",
	.parameters_json = I2C_READ_PARAMS,
	.execute = i2c_read_exec,
};

const tool_t HW_TOOLS_I2C_WRITE = {
	.name = "i2c_write",
	.description = "Write a byte payload to an I2C device register.",
	.parameters_json = I2C_WRITE_PARAMS,
	.execute = i2c_write_exec,
};

const tool_t HW_TOOLS_I2C_SCAN = {
	.name = "i2c_scan",
	.description = "Probe I2C bus addresses 0x03-0x77 and return responding devices.",
	.parameters_json = I2C_SCAN_PARAMS,
	.execute = i2c_scan_exec,
};
