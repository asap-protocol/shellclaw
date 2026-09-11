/**
 * @file hardware_libgpiod.h
 * @brief libgpiod v2 GPIO backend (Jetson / RPi).
 */

#ifndef SHELLCLAW_HARDWARE_LIBGPIOD_H
#define SHELLCLAW_HARDWARE_LIBGPIOD_H

#include "hardware/pin_table.h"
#include <stddef.h>

struct gpiod_chip;

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Bind the libgpiod backend to a board pin table.
 * @param table Pin table (must outlive the backend; not copied).
 * @return 0 on success, -1 on error.
 */
int hardware_libgpiod_init(const hardware_pin_table_t *table);

/** Release chips and line requests held by the backend. */
void hardware_libgpiod_shutdown(void);

/** Non-zero after successful #hardware_libgpiod_init. */
int hardware_libgpiod_is_available(void);

/**
 * Read a physical header pin (1–40).
 * @param pin Physical pin number.
 * @param value_out Set to 0 (LOW) or 1 (HIGH) on success.
 * @param errbuf Optional error buffer.
 * @param errbufsz Size of errbuf.
 * @return 0 on success, -1 on error.
 */
int hardware_gpio_read(int pin, int *value_out, char *errbuf, size_t errbufsz);

/**
 * Write a physical header pin (1–40).
 * @param pin Physical pin number.
 * @param value 0 = LOW, non-zero = HIGH.
 */
int hardware_gpio_write(int pin, int value, char *errbuf, size_t errbufsz);

/**
 * Configure pin direction. @p mode is "input" or "output".
 */
int hardware_gpio_mode(int pin, const char *mode, char *errbuf, size_t errbufsz);

/** Max gpiochip devices opened during one GPIO snapshot pass. */
#define HARDWARE_LIBGPIOD_SNAPSHOT_MAX_CHIPS 8

/**
 * Chip cache for batched read-only GPIO snapshot (one mutex hold per fill).
 * Only touch via #hardware_libgpiod_snapshot_begin / _pin_status / _end.
 */
typedef struct hardware_libgpiod_snapshot_ctx {
	struct gpiod_chip *chips[HARDWARE_LIBGPIOD_SNAPSHOT_MAX_CHIPS];
	unsigned int chip_nums[HARDWARE_LIBGPIOD_SNAPSHOT_MAX_CHIPS];
	int chip_count;
	int locked;
} hardware_libgpiod_snapshot_ctx_t;

/**
 * Begin a batched read-only snapshot (acquires GPIO mutex until end).
 * @return 0 on success, -1 when the backend is not ready.
 */
int hardware_libgpiod_snapshot_begin(hardware_libgpiod_snapshot_ctx_t *ctx);

/**
 * Read kernel direction and level for one GPIO row during a snapshot pass.
 * Output lines report mode @c "output" with an empty @p state_out (JSON null).
 * Input lines are requested as input only to read high/low.
 * @return 0 on success, -1 when the line cannot be queried.
 */
int hardware_libgpiod_snapshot_pin_status(hardware_libgpiod_snapshot_ctx_t *ctx,
					  const hardware_pin_entry_t *entry,
					  char *mode_out, size_t mode_sz, char *state_out,
					  size_t state_sz);

/** Close cached chips and release the GPIO mutex from snapshot_begin. */
void hardware_libgpiod_snapshot_end(hardware_libgpiod_snapshot_ctx_t *ctx);

/**
 * Override pin table for unit tests (pass NULL to clear).
 * Only available when compiling tests with SHELLCLAW_HARDWARE_LIBGPIOD_TEST.
 */
void hardware_libgpiod_set_pin_table_for_test(const hardware_pin_table_t *table);

/**
 * Test seam: succeed line request/set/get without a gpiochip, and count releases.
 * Only linked into test_hardware_libgpiod.
 */
void hardware_libgpiod_enable_fake_lines_for_test(int enable);
int hardware_libgpiod_release_count_for_test(void);
int hardware_libgpiod_held_count_for_test(void);

#ifdef __cplusplus
}
#endif

#endif /* SHELLCLAW_HARDWARE_LIBGPIOD_H */
