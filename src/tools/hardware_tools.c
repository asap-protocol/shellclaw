/**
 * @file hardware_tools.c
 * @brief Camera tool and hardware tool registry.
 */
#define _POSIX_C_SOURCE 200809L

#include "tools/hardware_tools.h"
#include "tools/hardware_tools_internal.h"
#include "core/config.h"
#include "hardware/hardware.h"
#include "tools/tool.h"
#include "cJSON.h"
#include <limits.h>
#include <stdio.h>
#include <string.h>

extern const tool_t HW_TOOLS_GPIO_READ;
extern const tool_t HW_TOOLS_GPIO_WRITE;
extern const tool_t HW_TOOLS_GPIO_MODE;
extern const tool_t HW_TOOLS_I2C_READ;
extern const tool_t HW_TOOLS_I2C_WRITE;
extern const tool_t HW_TOOLS_I2C_SCAN;

static const char CAMERA_CAPTURE_PARAMS[] =
	"{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\","
	"\"description\":\"Optional output JPEG path; auto temp file when omitted\"}}}";

static int camera_capture_exec(const char *args_json, char *result_buf, size_t max_len)
{
	cJSON *root = NULL;
	char path_buf[PATH_MAX];
	const char *output_path = NULL;
	const char *camera_type;
	const char *resolution;
	char result_path[512];
	char errbuf[256];
	board_id_t board;
	int quality;
	int rc;

	path_buf[0] = '\0';
	if (!hw_tools_enabled()) {
		snprintf(result_buf, max_len, "%s", HW_ERR_DISABLED);
		return -1;
	}
	if (args_json && args_json[0] != '\0') {
		cJSON *path_item;
		if (hw_tools_parse_root(args_json, &root, result_buf, max_len) != 0)
			return -1;
		path_item = cJSON_GetObjectItem(root, "path");
		if (path_item && cJSON_IsString(path_item) && path_item->valuestring[0])
			snprintf(path_buf, sizeof(path_buf), "%s", path_item->valuestring);
		cJSON_Delete(root);
		root = NULL;
	}
	if (path_buf[0] != '\0')
		output_path = path_buf;
	if (output_path && !hardware_camera_output_allowed(output_path)) {
		hw_tools_json_error(result_buf, max_len, "camera: path outside workspace");
		return -1;
	}
	if (!hardware_active_camera_backend() || !hardware_camera_is_available()) {
		snprintf(result_buf, max_len, "%s", HW_ERR_CAMERA);
		return -1;
	}
	board = hardware_active_board();
	camera_type = config_hardware_camera_type(g_hw_cfg);
	resolution = config_hardware_camera_resolution(g_hw_cfg);
	quality = config_hardware_camera_quality(g_hw_cfg);
	rc = hardware_camera_capture(board, camera_type, resolution, quality, 0, 0, output_path,
				     result_path, sizeof(result_path), errbuf, sizeof(errbuf));
	if (rc != 0) {
		hw_tools_json_error(result_buf, max_len, errbuf);
		return -1;
	}
	snprintf(result_buf, max_len, "{\"path\":\"%s\"}", result_path);
	return 0;
}

const tool_t HW_TOOLS_CAMERA_CAPTURE = {
	.name = "camera_capture",
	.description = "Capture a still JPEG frame using the board camera backend (CSI or USB).",
	.parameters_json = CAMERA_CAPTURE_PARAMS,
	.execute = camera_capture_exec,
};

static const tool_t *const HARDWARE_TOOLS[] = {
	&HW_TOOLS_GPIO_READ,
	&HW_TOOLS_GPIO_WRITE,
	&HW_TOOLS_GPIO_MODE,
	&HW_TOOLS_I2C_READ,
	&HW_TOOLS_I2C_WRITE,
	&HW_TOOLS_I2C_SCAN,
	&HW_TOOLS_CAMERA_CAPTURE,
};

static const size_t HARDWARE_TOOL_COUNT =
	sizeof(HARDWARE_TOOLS) / sizeof(HARDWARE_TOOLS[0]);

void tool_hardware_set_config(const config_t *cfg)
{
	g_hw_cfg = cfg;
	if (cfg && config_workspace_only(cfg))
		hardware_camera_set_workspace(config_workspace_path(cfg));
	else
		hardware_camera_set_workspace(NULL);
}

size_t tool_hardware_get_all(const tool_t **out, size_t max_count)
{
	size_t i;
	size_t n = 0;

	if (!out || max_count == 0 || !hw_tools_enabled())
		return 0;
	for (i = 0; i < HARDWARE_TOOL_COUNT && n < max_count; i++)
		out[n++] = HARDWARE_TOOLS[i];
	return n;
}
