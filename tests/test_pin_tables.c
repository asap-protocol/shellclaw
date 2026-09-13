/**
 * @file test_pin_tables.c
 * @brief Validates Jetson and RPi 40-pin header tables.
 */

#include "test_runner.h"
#include "hardware/boards/jetson_orin_nano.h"
#include "hardware/boards/rpi_zero2w.h"
#include <stdio.h>

static int table_is_valid(const hardware_pin_table_t *table, const char *name)
{
	int i;
	int j;
	int expect_physical = 1;
	if (!table || !table->entries) {
		fprintf(stderr, "FAIL: %s table missing\n", name);
		return 1;
	}
	if (table->count != HARDWARE_HEADER_PIN_COUNT) {
		fprintf(stderr, "FAIL: %s count %d expected %d\n", name, table->count,
			HARDWARE_HEADER_PIN_COUNT);
		return 1;
	}
	for (i = 0; i < table->count; i++) {
		const hardware_pin_entry_t *a = &table->entries[i];
		if (a->physical_pin != expect_physical) {
			fprintf(stderr,
				"FAIL: %s pin[%d] physical_pin %d expected %d\n", name, i,
				a->physical_pin, expect_physical);
			return 1;
		}
		expect_physical++;
		for (j = i + 1; j < table->count; j++) {
			const hardware_pin_entry_t *b = &table->entries[j];
			if (a->gpiochip_num == b->gpiochip_num &&
			    a->line_num == b->line_num) {
				fprintf(stderr,
					"FAIL: %s duplicate gpiochip%u line %u at physical %d and %d\n",
					name, a->gpiochip_num, a->line_num, a->physical_pin,
					b->physical_pin);
				return 1;
			}
		}
	}
	return 0;
}

static int test_jetson_pin_table(void)
{
	ASSERT(table_is_valid(&jetson_orin_nano_pin_table, "jetson_orin_nano") == 0);
	ASSERT(jetson_orin_nano_pin_table.entries[2].physical_pin == 3);
	ASSERT(jetson_orin_nano_pin_table.entries[2].sfio_flag == 1);
	ASSERT(jetson_orin_nano_pin_table.entries[2].line_num == 2u);
	ASSERT(jetson_orin_nano_pin_table.entries[32].physical_pin == 33);
	ASSERT(jetson_orin_nano_pin_table.entries[32].line_num == 43u);
	ASSERT(jetson_orin_nano_pin_table.entries[32].sfio_flag == 0);
	return 0;
}

static int test_rpi_pin_table(void)
{
	ASSERT(table_is_valid(&rpi_zero2w_pin_table, "rpi_zero2w") == 0);
	ASSERT(rpi_zero2w_pin_table.entries[10].physical_pin == 11);
	ASSERT(rpi_zero2w_pin_table.entries[10].line_num == 17u);
	ASSERT(rpi_zero2w_pin_table.entries[10].sfio_flag == 0);
	ASSERT(rpi_zero2w_pin_table.entries[2].sfio_flag == 1);
	ASSERT(rpi_zero2w_pin_table.entries[23].physical_pin == 24);
	ASSERT(rpi_zero2w_pin_table.entries[23].sfio_flag == 1);
	ASSERT(rpi_zero2w_pin_table.entries[25].physical_pin == 26);
	ASSERT(rpi_zero2w_pin_table.entries[25].sfio_flag == 1);
	return 0;
}

int main(void)
{
	RUN(test_jetson_pin_table());
	RUN(test_rpi_pin_table());
	printf("test_pin_tables: all tests passed\n");
	return 0;
}
