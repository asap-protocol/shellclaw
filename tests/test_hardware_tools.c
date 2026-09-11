/**
 * @file test_hardware_tools.c
 * @brief hardware tool executor tests: disabled path, JSON validation, GPIO/I2C guards.
 */
#define _POSIX_C_SOURCE 200809L

#include "test_runner.h"
#include "tools/tool.h"
#include "tools/hardware_tools.h"
#include "core/config.h"
#include "hardware/hardware.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern const tool_t HW_TOOLS_GPIO_READ;
extern const tool_t HW_TOOLS_GPIO_WRITE;
extern const tool_t HW_TOOLS_GPIO_MODE;
extern const tool_t HW_TOOLS_I2C_READ;
extern const tool_t HW_TOOLS_I2C_WRITE;
extern const tool_t HW_TOOLS_CAMERA_CAPTURE;

/* Stubs so this test links without the full tool dependency graph. */
const tool_t *tool_shell_get(void) { return NULL; }
void tool_shell_set_config(const config_t *cfg) { (void)cfg; }
const tool_t *tool_web_search_get(void) { return NULL; }
void tool_web_search_set_config(const config_t *cfg) { (void)cfg; }
const tool_t *tool_file_get(void) { return NULL; }
void tool_file_set_config(const config_t *cfg) { (void)cfg; }
const tool_t *tool_cron_get(void) { return NULL; }
const tool_t *tool_context_get(void) { return NULL; }
void tool_context_set_config(const config_t *cfg) { (void)cfg; }
const tool_t *tool_asap_invoke_get(void) { return NULL; }
void tool_asap_invoke_set_config(const config_t *cfg) { (void)cfg; }

static int mock_open(const char *path, int flags)
{
	(void)path;
	(void)flags;
	return 42;
}

static int mock_close(int fd)
{
	(void)fd;
	return 0;
}

static ssize_t mock_read(int fd, void *buf, size_t count)
{
	(void)fd;
	if (count > 0)
		((uint8_t *)buf)[0] = 0xab;
	return count > 0 ? 1 : 0;
}

static ssize_t mock_write(int fd, const void *buf, size_t count)
{
	(void)fd;
	(void)buf;
	return (ssize_t)count;
}

static int mock_ioctl(int fd, unsigned long request, void *arg)
{
	(void)fd;
	if (request == HARDWARE_I2C_IOCTL_SLAVE)
		return 0;
	if (request == HARDWARE_I2C_IOCTL_SMBUS)
		return -1;
	(void)arg;
	errno = EINVAL;
	return -1;
}

static const hardware_i2c_syscalls_t s_mock_ops = {
	.open_fn = mock_open,
	.close_fn = mock_close,
	.read_fn = mock_read,
	.write_fn = mock_write,
	.ioctl_fn = mock_ioctl,
};

static int load_cfg(const char *toml_body, config_t **cfg_out)
{
	char path[128];
	FILE *f;
	char errbuf[256];

	ASSERT(test_runner_mkstemp_path("shellclaw_hw_tools_cfg", path, sizeof(path)) == 0);
	f = fopen(path, "w");
	ASSERT(f);
	fprintf(f, "%s", toml_body);
	fclose(f);
	ASSERT(config_load(path, cfg_out, errbuf, sizeof(errbuf)) == 0);
	remove(path);
	return 0;
}

static int test_disabled_returns_error(void)
{
	config_t *cfg = NULL;
	char result[256];
	int rc;

	RUN(load_cfg("[agent]\nmodel = \"test\"\n\n[hardware]\nenabled = false\n", &cfg));
	tool_hardware_set_config(cfg);
	rc = HW_TOOLS_GPIO_READ.execute("{\"pin\":11}", result, sizeof(result));
	ASSERT(rc == -1);
	ASSERT(strstr(result, "hardware disabled") != NULL);
	config_free(cfg);
	return 0;
}

static int test_gpio_invalid_json(void)
{
	config_t *cfg = NULL;
	char result[256];
	int rc;

	RUN(load_cfg("[agent]\nmodel = \"test\"\n\n[hardware]\nenabled = true\nboard = \"stub\"\n",
		     &cfg));
	tool_hardware_set_config(cfg);
	rc = HW_TOOLS_GPIO_READ.execute("not-json", result, sizeof(result));
	ASSERT(rc == -1);
	ASSERT(strstr(result, "invalid JSON") != NULL);
	config_free(cfg);
	return 0;
}

static int test_gpio_pin_out_of_range(void)
{
	config_t *cfg = NULL;
	char result[256];
	int rc;

	RUN(load_cfg("[agent]\nmodel = \"test\"\n\n[hardware]\nenabled = true\nboard = \"stub\"\n",
		     &cfg));
	tool_hardware_set_config(cfg);
	rc = HW_TOOLS_GPIO_READ.execute("{\"pin\":0}", result, sizeof(result));
	ASSERT(rc == -1);
	ASSERT(strstr(result, "pin must be 1-40") != NULL);
	rc = HW_TOOLS_GPIO_READ.execute("{\"pin\":41}", result, sizeof(result));
	ASSERT(rc == -1);
	ASSERT(strstr(result, "pin must be 1-40") != NULL);
	config_free(cfg);
	return 0;
}

static int test_i2c_read_success_with_mock(void)
{
	config_t *cfg = NULL;
	char result[256];
	int rc;

	RUN(load_cfg("[agent]\nmodel = \"test\"\n\n[hardware]\nenabled = true\nboard = \"rpi\"\n",
		     &cfg));
	hardware_init(cfg);
	tool_hardware_set_config(cfg);
	hardware_i2c_set_syscalls_for_test(&s_mock_ops);
	rc = HW_TOOLS_I2C_READ.execute("{\"addr\":16,\"reg\":1,\"len\":1}", result,
				       sizeof(result));
	ASSERT(rc == 0);
	ASSERT(strstr(result, "\"data\"") != NULL);
	ASSERT(strstr(result, "171") != NULL);
	config_free(cfg);
	hardware_i2c_set_syscalls_for_test(NULL);
	return 0;
}

static int test_i2c_invalid_addr(void)
{
	config_t *cfg = NULL;
	char result[256];
	int rc;

	RUN(load_cfg("[agent]\nmodel = \"test\"\n\n[hardware]\nenabled = true\nboard = \"rpi\"\n",
		     &cfg));
	hardware_init(cfg);
	tool_hardware_set_config(cfg);
	hardware_i2c_set_syscalls_for_test(&s_mock_ops);
	rc = HW_TOOLS_I2C_READ.execute("{\"bus\":1,\"addr\":2,\"reg\":0,\"len\":1}", result,
				       sizeof(result));
	ASSERT(rc == -1);
	ASSERT(strstr(result, "0x03-0x77") != NULL);
	config_free(cfg);
	hardware_i2c_set_syscalls_for_test(NULL);
	return 0;
}

static int test_i2c_default_bus_from_board(void)
{
	config_t *cfg = NULL;
	char result[256];
	int rc;

	RUN(load_cfg("[agent]\nmodel = \"test\"\n\n[hardware]\nenabled = true\nboard = \"jetson\"\n",
		     &cfg));
	hardware_init(cfg);
	tool_hardware_set_config(cfg);
	hardware_i2c_set_syscalls_for_test(&s_mock_ops);
	rc = HW_TOOLS_I2C_WRITE.execute("{\"addr\":16,\"reg\":0,\"data\":[1]}", result,
					sizeof(result));
	ASSERT(rc == 0);
	ASSERT(strstr(result, "\"bus\":7") != NULL);
	config_free(cfg);
	hardware_i2c_set_syscalls_for_test(NULL);
	return 0;
}

static int test_i2c_rejects_out_of_range_bus(void)
{
	config_t *cfg = NULL;
	char result[256];
	int rc;

	RUN(load_cfg("[agent]\nmodel = \"test\"\n\n[hardware]\nenabled = true\nboard = \"jetson\"\n",
		     &cfg));
	hardware_init(cfg);
	tool_hardware_set_config(cfg);
	hardware_i2c_set_syscalls_for_test(&s_mock_ops);
	rc = HW_TOOLS_I2C_READ.execute("{\"bus\":256,\"addr\":16,\"reg\":0,\"len\":1}", result,
				       sizeof(result));
	ASSERT(rc == -1);
	ASSERT(strstr(result, "\"error\":\"bus must be 0-255\"") != NULL);
	rc = HW_TOOLS_I2C_READ.execute("{\"bus\":-1,\"addr\":16,\"reg\":0,\"len\":1}", result,
				       sizeof(result));
	ASSERT(rc == -1);
	ASSERT(strstr(result, "\"error\":\"bus must be 0-255\"") != NULL);
	config_free(cfg);
	hardware_i2c_set_syscalls_for_test(NULL);
	return 0;
}

static int test_gpio_mode_rejects_invalid_mode(void)
{
	config_t *cfg = NULL;
	char result[256];
	int rc;

	RUN(load_cfg("[agent]\nmodel = \"test\"\n\n[hardware]\nenabled = true\nboard = \"rpi\"\n",
		     &cfg));
	hardware_init(cfg);
	tool_hardware_set_config(cfg);
	rc = HW_TOOLS_GPIO_MODE.execute("{\"pin\":11,\"mode\":\"pwm\"}", result, sizeof(result));
	ASSERT(rc == -1);
	ASSERT(strstr(result, "input or output") != NULL);
	config_free(cfg);
	return 0;
}

static int test_gpio_mode_executes_json(void)
{
	config_t *cfg = NULL;
	char result[256];
	int rc;

	RUN(load_cfg("[agent]\nmodel = \"test\"\n\n[hardware]\nenabled = true\nboard = \"rpi\"\n",
		     &cfg));
	hardware_init(cfg);
	tool_hardware_set_config(cfg);
	rc = HW_TOOLS_GPIO_MODE.execute("{\"pin\":11,\"mode\":\"output\"}", result, sizeof(result));
	ASSERT(result[0] != '\0');
	if (rc == 0) {
		ASSERT(strstr(result, "\"mode\":\"output\"") != NULL);
		ASSERT(strstr(result, "\"pin\":11") != NULL);
	} else {
		ASSERT(strstr(result, "error") != NULL);
	}
	config_free(cfg);
	return 0;
}

static int test_gpio_write_rejects_non_binary_value(void)
{
	config_t *cfg = NULL;
	char result[256];
	int rc;

	RUN(load_cfg("[agent]\nmodel = \"test\"\n\n[hardware]\nenabled = true\nboard = \"stub\"\n",
		     &cfg));
	tool_hardware_set_config(cfg);
	rc = HW_TOOLS_GPIO_WRITE.execute("{\"pin\":11,\"value\":2}", result, sizeof(result));
	ASSERT(rc == -1);
	ASSERT(strstr(result, "value must be 0 or 1") != NULL);
	ASSERT(strstr(result, "got 2") != NULL);
	config_free(cfg);
	return 0;
}

static int test_camera_capture_rejects_path_outside_workspace(void)
{
	char ws[128];
	char toml[512];
	config_t *cfg = NULL;
	char result[256];
	int rc;

	ASSERT(test_runner_mkdtemp_path("shellclaw_hw_cam_ws", ws, sizeof(ws)) == 0);
	snprintf(toml, sizeof(toml),
		 "[agent]\nmodel = \"test\"\n\n[hardware]\nenabled = true\nboard = \"rpi\"\n\n"
		 "[sandbox]\nworkspace_only = true\nworkspace_path = \"%s\"\n",
		 ws);
	RUN(load_cfg(toml, &cfg));
	hardware_init(cfg);
	tool_hardware_set_config(cfg);
	rc = HW_TOOLS_CAMERA_CAPTURE.execute("{\"path\":\"/tmp/shellclaw_escape.jpg\"}",
					     result, sizeof(result));
	ASSERT(rc == -1);
	ASSERT(strstr(result, "workspace") != NULL);
	config_free(cfg);
	rmdir(ws);
	return 0;
}

int main(void)
{
	RUN(test_disabled_returns_error());
	RUN(test_gpio_invalid_json());
	RUN(test_gpio_pin_out_of_range());
	RUN(test_gpio_mode_rejects_invalid_mode());
	RUN(test_gpio_mode_executes_json());
	RUN(test_gpio_write_rejects_non_binary_value());
	RUN(test_camera_capture_rejects_path_outside_workspace());
	RUN(test_i2c_read_success_with_mock());
	RUN(test_i2c_invalid_addr());
	RUN(test_i2c_default_bus_from_board());
	RUN(test_i2c_rejects_out_of_range_bus());
	printf("test_hardware_tools: all tests passed\n");
	return 0;
}
