/**
 * @file test_routes_hardware.c
 * @brief Unit tests for /api/hardware/... route handlers (routes_hardware_dispatch).
 */
#define _POSIX_C_SOURCE 200809L

#include "gateway/routes_hardware.h"
#include "hardware/hardware.h"
#include "hardware/hardware_tegrastats.h"
#include "core/config.h"
#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RESP_SZ 65536
#define HTTP_GET 1
#define HTTP_POST 2
#define ASSERT(c)                                                                          \
	do {                                                                               \
		if (!(c)) {                                                                \
			fprintf(stderr, "FAIL: %s:%d %s\n", __FILE__, __LINE__, #c);       \
			return 1;                                                          \
		}                                                                          \
	} while (0)

/** First field must match http_server_ctx_t::cfg for dispatch tests. */
typedef struct {
	const config_t *cfg;
} test_hw_ctx_t;

static test_hw_ctx_t g_ctx;

static int write_toml(const char *path, const char *hardware_section)
{
	FILE *f = fopen(path, "w");

	if (!f)
		return -1;
	fprintf(f, "[agent]\nmodel = \"test\"\n\n[hardware]\n%s", hardware_section);
	fclose(f);
	return 0;
}

static int load_cfg(const char *path, config_t **cfg_out)
{
	char errbuf[256];

	if (config_load(path, cfg_out, errbuf, sizeof(errbuf)) != 0)
		return -1;
	return 0;
}

static int dispatch_get(const char *path, char *buf, size_t size, int *status)
{
	const char *uri = path;
	int uri_len = (int)strlen(path);

	return routes_hardware_dispatch((http_server_ctx_t *)&g_ctx, NULL, HTTP_GET, uri,
					uri_len, buf, size, status);
}

static int dispatch_post(const char *path, char *buf, size_t size, int *status)
{
	const char *uri = path;
	int uri_len = (int)strlen(path);

	return routes_hardware_dispatch((http_server_ctx_t *)&g_ctx, NULL, HTTP_POST, uri,
					uri_len, buf, size, status);
}

static int setup_board(const char *hardware_section)
{
	static char path[128];
	config_t *cfg = NULL;

	snprintf(path, sizeof(path), "/tmp/shellclaw_test_routes_hw_%s.toml",
		 hardware_section);
	ASSERT(write_toml(path, hardware_section) == 0);
	ASSERT(load_cfg(path, &cfg) == 0);
	g_ctx.cfg = cfg;
	ASSERT(hardware_init(cfg) == 0);
	return 0;
}

static void teardown_board(void)
{
	if (g_ctx.cfg) {
		config_free((config_t *)g_ctx.cfg);
		g_ctx.cfg = NULL;
	}
}

static int parse_ok_body(char *buf, int status, cJSON **root_out)
{
	cJSON *root;

	if (status != 200)
		return 1;
	root = cJSON_Parse(buf);
	if (!root)
		return 1;
	*root_out = root;
	return 0;
}

static int assert_deferred_v12(cJSON *root, const char *expected_message)
{
	cJSON *st;
	cJSON *msg;

	st = cJSON_GetObjectItemCaseSensitive(root, "status");
	msg = cJSON_GetObjectItemCaseSensitive(root, "message");
	if (!cJSON_IsString(st) || strcmp(st->valuestring, "deferred_v12") != 0)
		return 1;
	if (!cJSON_IsString(msg) || strcmp(msg->valuestring, expected_message) != 0)
		return 1;
	return 0;
}

static int test_board_schema_jetson(void)
{
	char buf[RESP_SZ];
	int status = 0;
	cJSON *root = NULL;
	cJSON *backends;
	cJSON *gpio;
	cJSON *i2c;
	cJSON *camera;

	if (setup_board("enabled = true\nboard = \"jetson\"\n") != 0)
		return 1;
	ASSERT(dispatch_get("/api/hardware/board", buf, sizeof(buf), &status) == 1);
	ASSERT(parse_ok_body(buf, status, &root) == 0);
	ASSERT(strcmp(cJSON_GetObjectItemCaseSensitive(root, "id")->valuestring,
		      "jetson_orin_nano") == 0);
	backends = cJSON_GetObjectItemCaseSensitive(root, "backends");
	ASSERT(cJSON_IsObject(backends));
	gpio = cJSON_GetObjectItemCaseSensitive(backends, "gpio");
	i2c = cJSON_GetObjectItemCaseSensitive(backends, "i2c");
	camera = cJSON_GetObjectItemCaseSensitive(backends, "camera");
	ASSERT(cJSON_IsString(gpio));
	ASSERT(cJSON_IsString(i2c));
	ASSERT(cJSON_IsString(camera));
	cJSON_Delete(root);
	teardown_board();
	return 0;
}

static int test_gpio_schema(void)
{
	char buf[RESP_SZ];
	int status = 0;
	cJSON *root = NULL;
	cJSON *pins;
	cJSON *first;

	if (setup_board("enabled = true\nboard = \"jetson\"\n") != 0)
		return 1;
	ASSERT(dispatch_get("/api/hardware/gpio", buf, sizeof(buf), &status) == 1);
	ASSERT(parse_ok_body(buf, status, &root) == 0);
	pins = cJSON_GetObjectItemCaseSensitive(root, "pins");
	ASSERT(cJSON_IsArray(pins));
	ASSERT(cJSON_GetArraySize(pins) == 40);
	first = cJSON_GetArrayItem(pins, 0);
	ASSERT(cJSON_GetObjectItemCaseSensitive(first, "pin") != NULL);
	ASSERT(cJSON_GetObjectItemCaseSensitive(first, "mode") != NULL);
	ASSERT(cJSON_GetObjectItemCaseSensitive(first, "sfio") != NULL);
	cJSON_Delete(root);
	teardown_board();
	return 0;
}

static int test_i2c_scan_unavailable_on_stub(void)
{
	char buf[RESP_SZ];
	int status = 0;

	if (setup_board("enabled = false\n") != 0)
		return 1;
	ASSERT(dispatch_get("/api/hardware/i2c-scan", buf, sizeof(buf), &status) == 1);
	ASSERT(status == 503);
	ASSERT(strstr(buf, "error") != NULL);
	teardown_board();
	return 0;
}

static const char ROUTES_GPU_SAMPLE_LINE[] =
	"05-23-2026 12:00:00 RAM 1000/7620MB (lfb 1x4MB) GR3D_FREQ 8%@[900,900] gpu@41.0C";

static int routes_collect_gpu_sample(char *linebuf, size_t linebufsz, char *errbuf,
				     size_t errbufsz)
{
	(void)errbuf;
	(void)errbufsz;
	snprintf(linebuf, linebufsz, "%s", ROUTES_GPU_SAMPLE_LINE);
	return 0;
}

static int test_gpu_jetson_available(void)
{
	char buf[RESP_SZ];
	int status = 0;
	cJSON *root = NULL;
	cJSON *avail;

	hardware_tegrastats_set_collect_for_test(routes_collect_gpu_sample);
	hardware_tegrastats_set_power_mode_for_test("MAXN");
	hardware_tegrastats_set_llama_running_for_test(0);
	if (setup_board("enabled = true\nboard = \"jetson\"\n") != 0)
		return 1;
	ASSERT(dispatch_get("/api/hardware/gpu", buf, sizeof(buf), &status) == 1);
	ASSERT(parse_ok_body(buf, status, &root) == 0);
	avail = cJSON_GetObjectItemCaseSensitive(root, "available");
	ASSERT(cJSON_IsTrue(avail));
	ASSERT(cJSON_GetObjectItemCaseSensitive(root, "gpu_usage") != NULL);
	ASSERT(cJSON_GetObjectItemCaseSensitive(root, "llama_server") != NULL);
	cJSON_Delete(root);
	hardware_tegrastats_set_collect_for_test(NULL);
	hardware_tegrastats_set_power_mode_for_test(NULL);
	hardware_tegrastats_set_llama_running_for_test(-1);
	teardown_board();
	return 0;
}

static int test_gpu_non_jetson(void)
{
	char buf[RESP_SZ];
	int status = 0;
	cJSON *root = NULL;
	cJSON *avail;

	if (setup_board("enabled = true\nboard = \"rpi\"\n") != 0)
		return 1;
	ASSERT(dispatch_get("/api/hardware/gpu", buf, sizeof(buf), &status) == 1);
	ASSERT(parse_ok_body(buf, status, &root) == 0);
	avail = cJSON_GetObjectItemCaseSensitive(root, "available");
	ASSERT(cJSON_IsFalse(avail));
	ASSERT(cJSON_IsString(cJSON_GetObjectItemCaseSensitive(root, "reason")));
	cJSON_Delete(root);
	teardown_board();
	return 0;
}

static int test_deferred_stubs(void)
{
	char buf[RESP_SZ];
	int status = 0;
	cJSON *root = NULL;

	if (setup_board("enabled = false\n") != 0)
		return 1;
	ASSERT(dispatch_get("/api/hardware/sensors", buf, sizeof(buf), &status) == 1);
	ASSERT(parse_ok_body(buf, status, &root) == 0);
	ASSERT(assert_deferred_v12(root,
				  "sensor decoders ship in v1.2 (Phase 7)") == 0);
	cJSON_Delete(root);

	status = 0;
	root = NULL;
	ASSERT(dispatch_post("/api/hardware/camera/snapshot", buf, sizeof(buf), &status) ==
	       1);
	ASSERT(parse_ok_body(buf, status, &root) == 0);
	ASSERT(assert_deferred_v12(
		       root, "camera image return path ships in v1.2 (Phase 7)") == 0);
	cJSON_Delete(root);
	teardown_board();
	return 0;
}

static int test_method_not_allowed(void)
{
	char buf[RESP_SZ];
	int status = 0;

	if (setup_board("enabled = true\nboard = \"jetson\"\n") != 0)
		return 1;
	ASSERT(dispatch_post("/api/hardware/board", buf, sizeof(buf), &status) == 1);
	ASSERT(status == 405);
	ASSERT(strstr(buf, "Method not allowed") != NULL);

	status = 0;
	ASSERT(dispatch_get("/api/hardware/camera/snapshot", buf, sizeof(buf), &status) ==
	       1);
	ASSERT(status == 405);
	ASSERT(strstr(buf, "Method not allowed") != NULL);
	teardown_board();
	return 0;
}

static int test_unknown_path_not_handled(void)
{
	char buf[RESP_SZ];
	int status = 0;

	g_ctx.cfg = NULL;
	ASSERT(dispatch_get("/api/hardware/unknown", buf, sizeof(buf), &status) == 0);
	return 0;
}

int main(void)
{
	int failures = 0;

	memset(&g_ctx, 0, sizeof(g_ctx)); /* NOLINT */
	if (test_board_schema_jetson() != 0)
		failures++;
	if (test_gpio_schema() != 0)
		failures++;
	if (test_i2c_scan_unavailable_on_stub() != 0)
		failures++;
	if (test_gpu_jetson_available() != 0)
		failures++;
	if (test_gpu_non_jetson() != 0)
		failures++;
	if (test_deferred_stubs() != 0)
		failures++;
	if (test_method_not_allowed() != 0)
		failures++;
	if (test_unknown_path_not_handled() != 0)
		failures++;
	if (failures == 0) {
		printf("test_routes_hardware: all tests passed\n");
		return 0;
	}
	fprintf(stderr, "test_routes_hardware: %d test(s) failed\n", failures);
	return 1;
}
