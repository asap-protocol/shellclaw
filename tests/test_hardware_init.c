/**
 * @file test_hardware_init.c
 * @brief Unit tests for hardware_init backend selection.
 */
#define _POSIX_C_SOURCE 200809L

#include "core/config.h"
#include "hardware/board_detect.h"
#include "hardware/hardware.h"
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

static int write_toml(const char *path, const char *hardware_section)
{
	FILE *f = fopen(path, "w");
	ASSERT(f);
	fprintf(f, "[agent]\nmodel = \"test\"\n\n[hardware]\n%s", hardware_section);
	fclose(f);
	return 0;
}

static int load_cfg(const char *path, config_t **cfg_out)
{
	char errbuf[256];
	ASSERT(config_load(path, cfg_out, errbuf, sizeof(errbuf)) == 0);
	return 0;
}

static int test_disabled_binds_stub_only(void)
{
	const char *path = "/tmp/shellclaw_test_hw_init_disabled.toml";
	config_t *cfg = NULL;

	unsetenv("SHELLCLAW_BOARD");
	board_detect_set_path_for_test(NULL);
	ASSERT(write_toml(path, "enabled = false\n") == 0);
	ASSERT(load_cfg(path, &cfg) == 0);
	ASSERT(hardware_init(cfg) == 0);
	ASSERT(hardware_active_board() == BOARD_STUB);
	ASSERT(hardware_active_gpio_backend() == HARDWARE_GPIO_BACKEND_STUB);
	ASSERT(hardware_stub_is_available() == 1);
	ASSERT(hardware_active_i2c_backend() == 0);
	ASSERT(hardware_active_camera_backend() == 0);
	config_free(cfg);
	remove(path);
	return 0;
}

static int test_enabled_stub_board(void)
{
	const char *path = "/tmp/shellclaw_test_hw_init_stub.toml";
	config_t *cfg = NULL;

	unsetenv("SHELLCLAW_BOARD");
	board_detect_set_path_for_test(NULL);
	ASSERT(write_toml(path, "enabled = true\nboard = \"stub\"\n") == 0);
	ASSERT(load_cfg(path, &cfg) == 0);
	ASSERT(hardware_init(cfg) == 0);
	ASSERT(hardware_active_board() == BOARD_STUB);
	ASSERT(hardware_active_gpio_backend() == HARDWARE_GPIO_BACKEND_UNAVAILABLE);
	ASSERT(hardware_active_i2c_backend() == 1);
	ASSERT(hardware_active_camera_backend() == 1);
	ASSERT(hardware_gpio_test_pin(cfg) == 0);
	config_free(cfg);
	remove(path);
	return 0;
}

static int test_enabled_jetson_board(void)
{
	const char *path = "/tmp/shellclaw_test_hw_init_jetson.toml";
	config_t *cfg = NULL;

	unsetenv("SHELLCLAW_BOARD");
	board_detect_set_path_for_test(NULL);
	ASSERT(write_toml(path, "enabled = true\nboard = \"jetson\"\n") == 0);
	ASSERT(load_cfg(path, &cfg) == 0);
	ASSERT(hardware_init(cfg) == 0);
	ASSERT(hardware_active_board() == BOARD_JETSON_ORIN_NANO);
#ifdef HAVE_LIBGPIOD
	ASSERT(hardware_active_gpio_backend() == HARDWARE_GPIO_BACKEND_LIBGPIOD);
	ASSERT(hardware_libgpiod_is_available() == 1);
#else
	ASSERT(hardware_active_gpio_backend() == HARDWARE_GPIO_BACKEND_UNAVAILABLE);
#endif
	ASSERT(hardware_active_i2c_backend() == 1);
	ASSERT(hardware_active_camera_backend() == 1);
	ASSERT(hardware_gpio_test_pin(cfg) == 33);
	config_free(cfg);
	remove(path);
	return 0;
}

static int test_enabled_rpi_board(void)
{
	const char *path = "/tmp/shellclaw_test_hw_init_rpi.toml";
	config_t *cfg = NULL;

	unsetenv("SHELLCLAW_BOARD");
	board_detect_set_path_for_test(NULL);
	ASSERT(write_toml(path, "enabled = true\nboard = \"rpi\"\n") == 0);
	ASSERT(load_cfg(path, &cfg) == 0);
	ASSERT(hardware_init(cfg) == 0);
	ASSERT(hardware_active_board() == BOARD_RPI_ZERO2W);
#ifdef HAVE_LIBGPIOD
	ASSERT(hardware_active_gpio_backend() == HARDWARE_GPIO_BACKEND_LIBGPIOD);
	ASSERT(hardware_libgpiod_is_available() == 1);
#else
	ASSERT(hardware_active_gpio_backend() == HARDWARE_GPIO_BACKEND_UNAVAILABLE);
#endif
	ASSERT(hardware_active_i2c_backend() == 1);
	ASSERT(hardware_active_camera_backend() == 1);
	ASSERT(hardware_gpio_test_pin(cfg) == 11);
	config_free(cfg);
	remove(path);
	return 0;
}

static int test_resolve_i2c_bus(void)
{
	const char *path = "/tmp/shellclaw_test_hw_init_i2c.toml";
	config_t *cfg = NULL;

	unsetenv("SHELLCLAW_BOARD");
	ASSERT(write_toml(path, "enabled = true\nboard = \"jetson\"\n") == 0);
	ASSERT(load_cfg(path, &cfg) == 0);
	ASSERT(hardware_init(cfg) == 0);
	ASSERT(hardware_resolve_i2c_bus(cfg) == 7);
	config_free(cfg);
	ASSERT(write_toml(path, "enabled = true\nboard = \"jetson\"\ni2c_bus = 1\n") == 0);
	ASSERT(load_cfg(path, &cfg) == 0);
	ASSERT(hardware_init(cfg) == 0);
	ASSERT(hardware_resolve_i2c_bus(cfg) == 1);
	config_free(cfg);
	remove(path);
	return 0;
}

static int test_gpio_test_pin_override(void)
{
	const char *path = "/tmp/shellclaw_test_hw_init_pin.toml";
	config_t *cfg = NULL;

	unsetenv("SHELLCLAW_BOARD");
	ASSERT(write_toml(path, "enabled = true\nboard = \"jetson\"\ngpio_test_pin = 7\n") == 0);
	ASSERT(load_cfg(path, &cfg) == 0);
	ASSERT(hardware_init(cfg) == 0);
	ASSERT(hardware_gpio_test_pin(cfg) == 7);
	config_free(cfg);
	remove(path);
	return 0;
}

int main(void)
{
	RUN(test_disabled_binds_stub_only());
	RUN(test_enabled_stub_board());
	RUN(test_enabled_jetson_board());
	RUN(test_enabled_rpi_board());
	RUN(test_resolve_i2c_bus());
	RUN(test_gpio_test_pin_override());
	printf("test_hardware_init: all tests passed\n");
	return 0;
}
