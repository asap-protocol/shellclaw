/**
 * @file hardware_init.c
 * @brief Select GPIO/I2C/camera backends from config and board detection.
 */
#define _POSIX_C_SOURCE 200809L

#include "hardware/hardware.h"
#include "hardware/board_detect.h"
#include "hardware/boards/jetson_orin_nano.h"
#include "hardware/boards/rpi_zero2w.h"
#include "core/config.h"
#include <string.h>

#define GPIO_TEST_PIN_JETSON_DEFAULT 33
#define GPIO_TEST_PIN_RPI_DEFAULT    11

static board_id_t s_active_board = BOARD_UNKNOWN;
static hardware_gpio_backend_t s_gpio_backend = HARDWARE_GPIO_BACKEND_UNAVAILABLE;
static int s_i2c_active;
static int s_camera_active;

static board_id_t resolve_board(const config_t *cfg)
{
	board_id_t from_cfg;
	board_id_t detected;

	from_cfg = board_id_from_string(config_hardware_board(cfg));
	if (from_cfg != BOARD_UNKNOWN)
		return from_cfg;
	detected = board_detect();
	if (detected != BOARD_UNKNOWN)
		return detected;
	return BOARD_STUB;
}

static void shutdown_backends(void)
{
#ifdef HAVE_LIBGPIOD
	hardware_libgpiod_shutdown();
#endif
	hardware_i2c_shutdown();
	hardware_camera_shutdown();
}

static int bind_gpio_backend(board_id_t board)
{
#ifdef HAVE_LIBGPIOD
	const hardware_pin_table_t *table = NULL;

	if (board == BOARD_JETSON_ORIN_NANO)
		table = &jetson_orin_nano_pin_table;
	else if (board == BOARD_RPI_ZERO2W)
		table = &rpi_zero2w_pin_table;
	if (table != NULL && hardware_libgpiod_init(table) == 0) {
		s_gpio_backend = HARDWARE_GPIO_BACKEND_LIBGPIOD;
		return 0;
	}
#endif
	(void)board;
	s_gpio_backend = HARDWARE_GPIO_BACKEND_UNAVAILABLE;
	return 0;
}

static int bind_enabled_backends(const config_t *cfg, board_id_t board)
{
	s_active_board = board;
	s_gpio_backend = HARDWARE_GPIO_BACKEND_UNAVAILABLE;
	s_i2c_active = 0;
	s_camera_active = 0;

	if (board == BOARD_STUB || board == BOARD_UNKNOWN) {
		/* GPIO stays unavailable; I2C and camera still initialize. */
	} else {
		(void)bind_gpio_backend(board);
	}
	if (hardware_i2c_init() == 0)
		s_i2c_active = 1;
	if (hardware_camera_init() == 0)
		s_camera_active = 1;
	(void)cfg;
	return 0;
}

static int bind_disabled_backends(const config_t *cfg)
{
	s_active_board = BOARD_STUB;
	s_gpio_backend = HARDWARE_GPIO_BACKEND_STUB;
	s_i2c_active = 0;
	s_camera_active = 0;
	return hardware_stub_init(cfg);
}

/**
 * Initialize or rebind hardware backends from @p cfg.
 * Idempotent: safe to call again when config changes.
 */
int hardware_init(const config_t *cfg)
{
	shutdown_backends();
	if (cfg == NULL || !config_hardware_enabled(cfg))
		return bind_disabled_backends(cfg);
	return bind_enabled_backends(cfg, resolve_board(cfg));
}

board_id_t hardware_active_board(void)
{
	return s_active_board;
}

hardware_gpio_backend_t hardware_active_gpio_backend(void)
{
	return s_gpio_backend;
}

int hardware_active_i2c_backend(void)
{
	return s_i2c_active;
}

int hardware_active_camera_backend(void)
{
	return s_camera_active;
}

int hardware_gpio_test_pin(const config_t *cfg)
{
	board_id_t board;

	if (!cfg)
		return 0;
	if (config_hardware_has_gpio_test_pin(cfg))
		return config_hardware_gpio_test_pin(cfg);
	board = hardware_active_board();
	if (board == BOARD_JETSON_ORIN_NANO)
		return GPIO_TEST_PIN_JETSON_DEFAULT;
	if (board == BOARD_RPI_ZERO2W)
		return GPIO_TEST_PIN_RPI_DEFAULT;
	return 0;
}

int hardware_default_i2c_bus(board_id_t board)
{
	if (board == BOARD_JETSON_ORIN_NANO)
		return 7;
	if (board == BOARD_RPI_ZERO2W)
		return 1;
	return 1;
}

int hardware_resolve_i2c_bus(const config_t *cfg)
{
	if (cfg != NULL && config_hardware_has_i2c_bus(cfg))
		return config_hardware_i2c_bus(cfg);
	return hardware_default_i2c_bus(hardware_active_board());
}

const hardware_pin_table_t *hardware_active_pin_table(void)
{
	switch (hardware_active_board()) {
	case BOARD_JETSON_ORIN_NANO:
		return &jetson_orin_nano_pin_table;
	case BOARD_RPI_ZERO2W:
		return &rpi_zero2w_pin_table;
	case BOARD_STUB:
	case BOARD_UNKNOWN:
	default:
		return NULL;
	}
}
