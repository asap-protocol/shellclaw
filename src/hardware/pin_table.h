/**
 * @file pin_table.h
 * @brief Physical header pin mapping for GPIO backends (40-pin header).
 */

#ifndef SHELLCLAW_PIN_TABLE_H
#define SHELLCLAW_PIN_TABLE_H

#ifdef __cplusplus
extern "C" {
#endif

/** Number of pins on the standard 40-pin expansion header. */
#define HARDWARE_HEADER_PIN_COUNT 40

/** One row of a board pin table (physical pin 1–40). */
typedef struct hardware_pin_entry {
	int physical_pin;
	unsigned int gpiochip_num;
	unsigned int line_num;
	int sfio_flag;
	const char *label;
} hardware_pin_entry_t;

/** Board-specific pin table consumed by #hardware_libgpiod_init. */
typedef struct hardware_pin_table {
	const hardware_pin_entry_t *entries;
	int count;
} hardware_pin_table_t;

#ifdef __cplusplus
}
#endif

#endif /* SHELLCLAW_PIN_TABLE_H */
