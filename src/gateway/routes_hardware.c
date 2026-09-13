/**
 * @file routes_hardware.c
 * @brief Handlers for /api/hardware/... (board, GPIO, I2C, GPU; v1.2 stubs).
 */
#define _POSIX_C_SOURCE 200809L

#include "gateway/routes_hardware.h"
#include "gateway/http_lws.h"
#include "gateway/uri_match.h"
#include <string.h>
#include "gateway/routes.h"
#include "hardware/hardware.h"
#include "hardware/hardware_gpio_snapshot.h"
#include "hardware/hardware_tegrastats.h"
#include "hardware/board_detect.h"
#include "core/config.h"
#include "cJSON.h"

static const char *board_display_name(board_id_t id)
{
	switch (id) {
	case BOARD_JETSON_ORIN_NANO:
		return "Jetson Orin Nano Super";
	case BOARD_RPI_ZERO2W:
		return "Raspberry Pi Zero 2 W";
	case BOARD_STUB:
		return "Stub";
	case BOARD_UNKNOWN:
	default:
		return "Unknown";
	}
}

static const char *gpio_backend_string(hardware_gpio_backend_t backend)
{
	switch (backend) {
	case HARDWARE_GPIO_BACKEND_LIBGPIOD:
		return "libgpiod";
	case HARDWARE_GPIO_BACKEND_STUB:
		return "stub";
	case HARDWARE_GPIO_BACKEND_UNAVAILABLE:
	default:
		return "none";
	}
}

static const char *camera_backend_string(const config_t *cfg, board_id_t board)
{
	const char *camera_type;

	if (!hardware_active_camera_backend())
		return "none";
	if (!cfg)
		return "none";
	camera_type = config_hardware_camera_type(cfg);
	if (camera_type && strcmp(camera_type, "usb") == 0)
		return "v4l2";
	if (board == BOARD_JETSON_ORIN_NANO)
		return "nvargus";
	if (board == BOARD_RPI_ZERO2W)
		return "libcamera";
	return "none";
}

static void handle_hardware_board(const config_t *cfg, char *buf, size_t size, int *status)
{
	board_id_t board;
	cJSON *root;
	cJSON *backends;
	const char *id;
	const char *name;

	board = hardware_active_board();
	id = board_name(board);
	name = board_display_name(board);
	root = cJSON_CreateObject();
	if (!root) {
		json_error(buf, size, status, 500, "Internal error");
		return;
	}
	cJSON_AddItemToObject(root, "id", cJSON_CreateString(id));
	cJSON_AddItemToObject(root, "name", cJSON_CreateString(name));
	backends = cJSON_CreateObject();
	if (!backends) {
		cJSON_Delete(root);
		json_error(buf, size, status, 500, "Internal error");
		return;
	}
	cJSON_AddItemToObject(backends, "gpio",
			      cJSON_CreateString(gpio_backend_string(
				      hardware_active_gpio_backend())));
	cJSON_AddItemToObject(backends, "i2c",
			      cJSON_CreateString(hardware_active_i2c_backend() ? "linux"
									       : "none"));
	cJSON_AddItemToObject(backends, "camera",
			      cJSON_CreateString(camera_backend_string(cfg, board)));
	cJSON_AddItemToObject(root, "backends", backends);
	if (json_print_to_buf(root, buf, size, status) != 0)
		json_error(buf, size, status, 500, "Internal error");
	cJSON_Delete(root);
}

static void handle_hardware_gpio(char *buf, size_t size, int *status)
{
	cJSON *root;
	cJSON *pins;
	char errbuf[256];

	root = cJSON_CreateObject();
	if (!root) {
		json_error(buf, size, status, 500, "Internal error");
		return;
	}
	pins = cJSON_CreateArray();
	if (!pins) {
		cJSON_Delete(root);
		json_error(buf, size, status, 500, "Internal error");
		return;
	}
	if (hardware_gpio_snapshot_fill(pins, errbuf, sizeof(errbuf)) != 0) {
		cJSON_Delete(pins);
		cJSON_Delete(root);
		json_error(buf, size, status, 503, errbuf[0] ? errbuf : "GPIO unavailable");
		return;
	}
	cJSON_AddItemToObject(root, "pins", pins);
	if (json_print_to_buf(root, buf, size, status) != 0)
		json_error(buf, size, status, 500, "Internal error");
	cJSON_Delete(root);
}

static void handle_hardware_i2c_scan(const config_t *cfg, char *buf, size_t size, int *status)
{
	cJSON *root;
	cJSON *addrs_arr;
	uint8_t addrs[128];
	int count = 0;
	int bus;
	int i;
	char errbuf[256];
	int rc;

	if (!hardware_active_i2c_backend() || !hardware_i2c_is_available()) {
		json_error(buf, size, status, 503, "I2C backend not available");
		return;
	}
	bus = hardware_resolve_i2c_bus(cfg);
	rc = hardware_i2c_scan(bus, addrs, (int)sizeof(addrs), &count, errbuf, sizeof(errbuf));
	if (rc != 0) {
		json_error(buf, size, status, 503, errbuf[0] ? errbuf : "I2C scan failed");
		return;
	}
	root = cJSON_CreateObject();
	if (!root) {
		json_error(buf, size, status, 500, "Internal error");
		return;
	}
	cJSON_AddItemToObject(root, "bus", cJSON_CreateNumber((double)bus));
	addrs_arr = cJSON_CreateArray();
	if (!addrs_arr) {
		cJSON_Delete(root);
		json_error(buf, size, status, 500, "Internal error");
		return;
	}
	for (i = 0; i < count; i++)
		cJSON_AddItemToArray(addrs_arr, cJSON_CreateNumber((double)addrs[i]));
	cJSON_AddItemToObject(root, "addresses", addrs_arr);
	if (json_print_to_buf(root, buf, size, status) != 0)
		json_error(buf, size, status, 500, "Internal error");
	cJSON_Delete(root);
}

#define DEFERRED_V12_STATUS "deferred_v12"
#define DEFERRED_SENSORS_MSG "sensor decoders ship in v1.2 (Phase 7)"
#define DEFERRED_CAMERA_MSG "camera image return path ships in v1.2 (Phase 7)"

static void handle_hardware_deferred_v12(char *buf, size_t size, int *status,
					 const char *message)
{
	cJSON *root;

	root = cJSON_CreateObject();
	if (!root) {
		json_error(buf, size, status, 500, "Internal error");
		return;
	}
	cJSON_AddItemToObject(root, "status", cJSON_CreateString(DEFERRED_V12_STATUS));
	cJSON_AddItemToObject(root, "message", cJSON_CreateString(message));
	if (json_print_to_buf(root, buf, size, status) != 0)
		json_error(buf, size, status, 500, "Internal error");
	cJSON_Delete(root);
}

static void handle_hardware_gpu(char *buf, size_t size, int *status)
{
	board_id_t board;
	cJSON *root;
	char errbuf[256];

	board = hardware_active_board();
	root = cJSON_CreateObject();
	if (!root) {
		json_error(buf, size, status, 500, "Internal error");
		return;
	}
	if (board != BOARD_JETSON_ORIN_NANO) {
		cJSON_AddBoolToObject(root, "available", 0);
		cJSON_AddItemToObject(root, "reason",
				      cJSON_CreateString("non-jetson board"));
		if (json_print_to_buf(root, buf, size, status) != 0)
			json_error(buf, size, status, 500, "Internal error");
		cJSON_Delete(root);
		return;
	}
	if (hardware_jetson_gpu_json_fill(root, errbuf, sizeof(errbuf)) != 0) {
		cJSON_Delete(root);
		root = cJSON_CreateObject();
		if (!root) {
			json_error(buf, size, status, 500, "Internal error");
			return;
		}
		cJSON_AddBoolToObject(root, "available", 0);
		cJSON_AddItemToObject(root, "reason",
				      cJSON_CreateString(errbuf[0] ? errbuf
								     : "tegrastats unavailable"));
	}
	if (json_print_to_buf(root, buf, size, status) != 0)
		json_error(buf, size, status, 500, "Internal error");
	cJSON_Delete(root);
}

typedef void (*hw_get_fn)(const config_t *cfg, char *buf, size_t size, int *status);
typedef void (*hw_get_fn_no_cfg)(char *buf, size_t size, int *status);

typedef struct {
	const char *path;
	int needs_cfg;
	int post_only;
	union {
		hw_get_fn with_cfg;
		hw_get_fn_no_cfg no_cfg;
	} get;
	const char *deferred_msg;
} hw_route_t;

static void hw_dispatch_get(const hw_route_t *route, const config_t *cfg,
			    char *buf, size_t size, int *status)
{
	if (route->deferred_msg) {
		handle_hardware_deferred_v12(buf, size, status, route->deferred_msg);
		return;
	}
	if (route->needs_cfg)
		route->get.with_cfg(cfg, buf, size, status);
	else
		route->get.no_cfg(buf, size, status);
}

int routes_hardware_dispatch(http_server_ctx_t *ctx, struct lws *wsi, int method,
			     const char *uri, int uri_len, char *buf, size_t size,
			     int *status)
{
	static const hw_route_t routes[] = {
		{ "/api/hardware/board", 1, 0, { .with_cfg = handle_hardware_board }, NULL },
		{ "/api/hardware/gpio", 0, 0, { .no_cfg = handle_hardware_gpio }, NULL },
		{ "/api/hardware/i2c-scan", 1, 0, { .with_cfg = handle_hardware_i2c_scan }, NULL },
		{ "/api/hardware/gpu", 0, 0, { .no_cfg = handle_hardware_gpu }, NULL },
		{ "/api/hardware/sensors", 0, 0, { .no_cfg = NULL }, DEFERRED_SENSORS_MSG },
		{ "/api/hardware/camera/snapshot", 0, 1, { .no_cfg = NULL },
		  DEFERRED_CAMERA_MSG },
	};
	size_t i;

	(void)wsi;
	if (!ctx || !uri || uri_len <= 0 || !buf || !status)
		return 0;
	for (i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
		const hw_route_t *route = &routes[i];

		if (!uri_exact_eq(uri, uri_len, route->path))
			continue;
		if (route->post_only) {
			if (method == HTTP_POST)
				handle_hardware_deferred_v12(buf, size, status,
							     route->deferred_msg);
			else
				json_error(buf, size, status, 405, "Method not allowed");
			return 1;
		}
		if (method == HTTP_GET)
			hw_dispatch_get(route, ctx->cfg, buf, size, status);
		else
			json_error(buf, size, status, 405, "Method not allowed");
		return 1;
	}
	return 0;
}
