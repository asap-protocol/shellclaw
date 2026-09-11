/**
 * @file hardware_gpio_snapshot.c
 * @brief 40-pin header snapshot for GET /api/hardware/gpio.
 */
#define _POSIX_C_SOURCE 200809L

#include "hardware/hardware_gpio_snapshot.h"
#include "hardware/hardware.h"
#include "hardware/board_detect.h"
#include "cJSON.h"
#include <stdio.h>
#include <string.h>

/** Power/ground rows use line_num = 4000 + physical_pin (see board headers). */
#define HARDWARE_PWR_LINE_BASE 4000u

static const hardware_pin_entry_t *find_physical_pin(const hardware_pin_table_t *table,
						     int physical_pin)
{
	int i;

	if (!table || !table->entries || physical_pin < 1 ||
	    physical_pin > HARDWARE_HEADER_PIN_COUNT)
		return NULL;
	for (i = 0; i < table->count; i++) {
		if (table->entries[i].physical_pin == physical_pin)
			return &table->entries[i];
	}
	return NULL;
}

static int is_power_row(const hardware_pin_entry_t *entry)
{
	return entry != NULL && entry->sfio_flag != 0 &&
	       entry->line_num >= HARDWARE_PWR_LINE_BASE;
}

#ifdef HAVE_LIBGPIOD
static int append_pin_object(cJSON *pins_array, const hardware_pin_entry_t *entry,
			     hardware_libgpiod_snapshot_ctx_t *snap_ctx)
#else
static int append_pin_object(cJSON *pins_array, const hardware_pin_entry_t *entry)
#endif
{
	cJSON *obj;
	const char *mode = "unavailable";
#ifdef HAVE_LIBGPIOD
	char mode_buf[16];
	char state_buf[8];
#endif

	if (!entry)
		return -1;
	obj = cJSON_CreateObject();
	if (!obj)
		return -1;
	cJSON_AddItemToObject(obj, "pin", cJSON_CreateNumber(entry->physical_pin));
	if (entry->label)
		cJSON_AddItemToObject(obj, "label", cJSON_CreateString(entry->label));
	cJSON_AddItemToObject(obj, "sfio", cJSON_CreateBool(entry->sfio_flag ? 1 : 0));
	if (is_power_row(entry) || entry->sfio_flag) {
		mode = "sfio";
		cJSON_AddItemToObject(obj, "mode", cJSON_CreateString(mode));
		cJSON_AddItemToObject(obj, "state", cJSON_CreateNull());
	} else if (hardware_active_gpio_backend() == HARDWARE_GPIO_BACKEND_LIBGPIOD) {
#ifdef HAVE_LIBGPIOD
		if (snap_ctx != NULL &&
		    hardware_libgpiod_snapshot_pin_status(snap_ctx, entry, mode_buf,
							  sizeof(mode_buf), state_buf,
							  sizeof(state_buf)) == 0) {
			cJSON_AddItemToObject(obj, "mode", cJSON_CreateString(mode_buf));
			if (state_buf[0] != '\0')
				cJSON_AddItemToObject(obj, "state",
						      cJSON_CreateString(state_buf));
			else
				cJSON_AddItemToObject(obj, "state", cJSON_CreateNull());
		} else
#endif
		{
			cJSON_AddItemToObject(obj, "mode", cJSON_CreateString(mode));
			cJSON_AddItemToObject(obj, "state", cJSON_CreateNull());
		}
	} else {
		cJSON_AddItemToObject(obj, "mode", cJSON_CreateString(mode));
		cJSON_AddItemToObject(obj, "state", cJSON_CreateNull());
	}
	cJSON_AddItemToArray(pins_array, obj);
	return 0;
}

int hardware_gpio_snapshot_fill(cJSON *pins_array, char *errbuf, size_t errbufsz)
{
	const hardware_pin_table_t *table;
	int physical;
#ifdef HAVE_LIBGPIOD
	hardware_libgpiod_snapshot_ctx_t snap_ctx;
	int snap_active = 0;
#endif

	if (!pins_array) {
		if (errbuf && errbufsz > 0)
			snprintf(errbuf, errbufsz, "gpio snapshot: pins_array is NULL");
		return -1;
	}
	table = hardware_active_pin_table();
	if (!table) {
		if (errbuf && errbufsz > 0)
			snprintf(errbuf, errbufsz,
				 "gpio snapshot: no pin table for board '%s'",
				 board_name(hardware_active_board()));
		return -1;
	}
#ifdef HAVE_LIBGPIOD
	if (hardware_active_gpio_backend() == HARDWARE_GPIO_BACKEND_LIBGPIOD &&
	    hardware_libgpiod_is_available() &&
	    hardware_libgpiod_snapshot_begin(&snap_ctx) == 0)
		snap_active = 1;
#endif
	for (physical = 1; physical <= HARDWARE_HEADER_PIN_COUNT; physical++) {
		const hardware_pin_entry_t *entry = find_physical_pin(table, physical);
		if (!entry) {
#ifdef HAVE_LIBGPIOD
			if (snap_active)
				hardware_libgpiod_snapshot_end(&snap_ctx);
#endif
			if (errbuf && errbufsz > 0)
				snprintf(errbuf, errbufsz,
					 "gpio snapshot: missing header pin %d in table",
					 physical);
			return -1;
		}
#ifdef HAVE_LIBGPIOD
		if (append_pin_object(pins_array, entry, snap_active ? &snap_ctx : NULL) !=
		    0) {
			if (snap_active)
				hardware_libgpiod_snapshot_end(&snap_ctx);
#else
		if (append_pin_object(pins_array, entry) != 0) {
#endif
			if (errbuf && errbufsz > 0)
				snprintf(errbuf, errbufsz, "gpio snapshot: out of memory");
			return -1;
		}
	}
#ifdef HAVE_LIBGPIOD
	if (snap_active)
		hardware_libgpiod_snapshot_end(&snap_ctx);
#endif
	return 0;
}
