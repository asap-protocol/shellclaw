/**
 * @file hardware.h
 * @brief Hardware abstraction (GPIO, I2C, camera) — Phase 5 backend API.
 */

#ifndef SHELLCLAW_HARDWARE_H
#define SHELLCLAW_HARDWARE_H

#include "hardware/board_detect.h"
#include "hardware/pin_table.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

struct config;
typedef struct config config_t;

/** Active GPIO backend selected by #hardware_init. */
typedef enum hardware_gpio_backend {
	HARDWARE_GPIO_BACKEND_UNAVAILABLE = 0,
	HARDWARE_GPIO_BACKEND_STUB,
	HARDWARE_GPIO_BACKEND_LIBGPIOD
} hardware_gpio_backend_t;

/**
 * Bind GPIO/I2C/camera backends from @p cfg (board override, detection, enabled flag).
 * Called once from the tool registry when config is applied.
 * @return 0 on success, -1 on fatal init error.
 */
int hardware_init(const config_t *cfg);

/** Board id bound by the last successful #hardware_init. */
board_id_t hardware_active_board(void);

/** GPIO backend bound by the last #hardware_init. */
hardware_gpio_backend_t hardware_active_gpio_backend(void);

/** Non-zero when the I2C backend was initialized by #hardware_init. */
int hardware_active_i2c_backend(void);

/** Non-zero when the camera backend was initialized by #hardware_init. */
int hardware_active_camera_backend(void);

/**
 * Pin table for the active board (Jetson / RPi), or NULL for stub/unknown.
 */
const hardware_pin_table_t *hardware_active_pin_table(void);

/**
 * Default GPIO test pin for the active board when config omits gpio_test_pin.
 * Jetson: physical pin 33; RPi: physical pin 11.
 */
int hardware_gpio_test_pin(const config_t *cfg);

/**
 * Default I2C bus number for @p board when config omits i2c_bus.
 * Jetson Orin Nano: 7; Raspberry Pi Zero 2 W: 1; other boards: 1.
 */
int hardware_default_i2c_bus(board_id_t board);

/**
 * I2C bus for tools and gateway: config override or per-board default.
 */
int hardware_resolve_i2c_bus(const config_t *cfg);

/**
 * No-op stub used when hardware is disabled in config.
 * @return 0 on success (stub always succeeds), -1 on fatal init error.
 */
int hardware_stub_init(const config_t *cfg);

/** Returns 1 when the stub layer reports hardware ready (always 1 for stub). */
int hardware_stub_is_available(void);

#ifdef HAVE_LIBGPIOD
#include "hardware/hardware_libgpiod.h"
#endif

#include "hardware/hardware_i2c.h"
#include "hardware/hardware_camera.h"

#ifdef __cplusplus
}
#endif

#endif /* SHELLCLAW_HARDWARE_H */
