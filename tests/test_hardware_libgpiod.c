/**
 * @file test_hardware_libgpiod.c
 * @brief libgpiod backend tests: smoke, SFIO rejection, optional gpio-mockup I/O.
 */

#include "test_runner.h"
#include <stdio.h>
#include <string.h>

#ifdef HAVE_LIBGPIOD
#include "hardware/hardware_libgpiod.h"
#include <gpiod.h>
#endif

static int test_compile_smoke(void)
{
#ifdef HAVE_LIBGPIOD
	printf("test_hardware_libgpiod: libgpiod present\n");
#else
	printf("test_hardware_libgpiod: libgpiod absent (compile-only smoke)\n");
#endif
	return 0;
}

#ifdef HAVE_LIBGPIOD

static const hardware_pin_entry_t s_test_pins[] = {
	{ 3, 0, 2, 1, "I2C_SDA" },
	{ 13, 0, 106, 0, "GPIO13" },
};

static const hardware_pin_table_t s_test_table = {
	.entries = s_test_pins,
	.count = 2,
};

static int test_sfio_rejection(void)
{
	char errbuf[128];
	int value = 0;
	hardware_libgpiod_set_pin_table_for_test(&s_test_table);
	ASSERT(hardware_libgpiod_init(&s_test_table) == 0);
	ASSERT(hardware_gpio_read(3, &value, errbuf, sizeof(errbuf)) != 0);
	ASSERT(strstr(errbuf, "SFIO") != NULL);
	ASSERT(strstr(errbuf, "pin 3") != NULL);
	ASSERT(hardware_gpio_write(3, 1, errbuf, sizeof(errbuf)) != 0);
	ASSERT(hardware_gpio_mode(3, "input", errbuf, sizeof(errbuf)) != 0);
	hardware_libgpiod_shutdown();
	hardware_libgpiod_set_pin_table_for_test(NULL);
	return 0;
}

static int test_gpio_requires_init(void)
{
	char errbuf[128];
	int value = 0;
	hardware_libgpiod_shutdown();
	ASSERT(hardware_gpio_read(13, &value, errbuf, sizeof(errbuf)) != 0);
	ASSERT(strstr(errbuf, "not initialized") != NULL);
	return 0;
}

static int test_gpio_validation_and_non_sfio_paths(void)
{
	char errbuf[128];
	int value = 0;
	hardware_libgpiod_set_pin_table_for_test(&s_test_table);
	ASSERT(hardware_libgpiod_init(&s_test_table) == 0);
	ASSERT(hardware_gpio_read(0, &value, errbuf, sizeof(errbuf)) != 0);
	ASSERT(strstr(errbuf, "out of range") != NULL);
	ASSERT(hardware_gpio_mode(13, "pwm", errbuf, sizeof(errbuf)) != 0);
	ASSERT(strstr(errbuf, "invalid mode") != NULL);
	ASSERT(hardware_gpio_read(13, &value, errbuf, sizeof(errbuf)) != 0);
	ASSERT(strstr(errbuf, "SFIO") == NULL);
	ASSERT(hardware_gpio_write(13, 1, errbuf, sizeof(errbuf)) != 0);
	ASSERT(hardware_gpio_mode(13, "output", errbuf, sizeof(errbuf)) != 0);
	hardware_libgpiod_shutdown();
	hardware_libgpiod_set_pin_table_for_test(NULL);
	return 0;
}

static int gpio_mockup_present(void)
{
	struct gpiod_chip *chip = gpiod_chip_open("/dev/gpiochip0");
	const char *label;
	struct gpiod_chip_info *info;
	if (!chip)
		return 0;
	info = gpiod_chip_get_info(chip);
	if (!info) {
		gpiod_chip_close(chip);
		return 0;
	}
	label = gpiod_chip_info_get_name(info);
	if (!label || strstr(label, "mockup") == NULL)
		label = gpiod_chip_info_get_label(info);
	gpiod_chip_info_free(info);
	gpiod_chip_close(chip);
	if (!label)
		return 0;
	return strstr(label, "mockup") != NULL;
}

static int test_mockup_snapshot_output_readonly(void)
{
	static const hardware_pin_entry_t pins[] = {
		{ 1, 0, 0, 0, "mock0" },
	};
	static const hardware_pin_table_t table = {
		.entries = pins,
		.count = 1,
	};
	hardware_libgpiod_snapshot_ctx_t ctx;
	char errbuf[128];
	char mode[16];
	char state[8];
	int value = 0;

	if (!gpio_mockup_present())
		return 0;
	ASSERT(hardware_libgpiod_init(&table) == 0);
	ASSERT(hardware_gpio_mode(1, "output", errbuf, sizeof(errbuf)) == 0);
	ASSERT(hardware_gpio_write(1, 1, errbuf, sizeof(errbuf)) == 0);
	ASSERT(hardware_libgpiod_snapshot_begin(&ctx) == 0);
	ASSERT(hardware_libgpiod_snapshot_pin_status(&ctx, &pins[0], mode, sizeof(mode),
						      state, sizeof(state)) == 0);
	ASSERT(strcmp(mode, "output") == 0);
	ASSERT(state[0] == '\0');
	hardware_libgpiod_snapshot_end(&ctx);
	ASSERT(hardware_gpio_read(1, &value, errbuf, sizeof(errbuf)) == 0);
	ASSERT(value == 1);
	hardware_libgpiod_shutdown();
	return 0;
}

static int test_mockup_read_write(void)
{
	static const hardware_pin_entry_t pins[] = {
		{ 1, 0, 0, 0, "mock0" },
	};
	static const hardware_pin_table_t table = {
		.entries = pins,
		.count = 1,
	};
	char errbuf[128];
	int value = 0;
	if (!gpio_mockup_present()) {
		printf("test_hardware_libgpiod: gpio-mockup not present, skipping I/O test\n");
		return 0;
	}
	ASSERT(hardware_libgpiod_init(&table) == 0);
	ASSERT(hardware_gpio_mode(1, "output", errbuf, sizeof(errbuf)) == 0);
	ASSERT(hardware_gpio_write(1, 1, errbuf, sizeof(errbuf)) == 0);
	ASSERT(hardware_gpio_mode(1, "input", errbuf, sizeof(errbuf)) == 0);
	ASSERT(hardware_gpio_read(1, &value, errbuf, sizeof(errbuf)) == 0);
	ASSERT(value == 1);
	ASSERT(hardware_gpio_write(1, 0, errbuf, sizeof(errbuf)) == 0);
	ASSERT(hardware_gpio_read(1, &value, errbuf, sizeof(errbuf)) == 0);
	ASSERT(value == 0);
	hardware_libgpiod_shutdown();
	return 0;
}

static int test_gpio_write_holds_line_request(void)
{
	char errbuf[128];

	hardware_libgpiod_enable_fake_lines_for_test(1);
	hardware_libgpiod_set_pin_table_for_test(&s_test_table);
	ASSERT(hardware_libgpiod_init(&s_test_table) == 0);
	ASSERT(hardware_gpio_write(13, 1, errbuf, sizeof(errbuf)) == 0);
	ASSERT(hardware_libgpiod_held_count_for_test() == 1);
	ASSERT(hardware_libgpiod_release_count_for_test() == 0);
	ASSERT(hardware_gpio_write(13, 0, errbuf, sizeof(errbuf)) == 0);
	ASSERT(hardware_libgpiod_held_count_for_test() == 1);
	ASSERT(hardware_libgpiod_release_count_for_test() == 0);
	ASSERT(hardware_gpio_mode(13, "input", errbuf, sizeof(errbuf)) == 0);
	ASSERT(hardware_libgpiod_held_count_for_test() == 0);
	ASSERT(hardware_libgpiod_release_count_for_test() >= 1);
	hardware_libgpiod_shutdown();
	hardware_libgpiod_enable_fake_lines_for_test(0);
	hardware_libgpiod_set_pin_table_for_test(NULL);
	return 0;
}

static int test_gpio_mode_output_holds_until_shutdown(void)
{
	char errbuf[128];

	hardware_libgpiod_enable_fake_lines_for_test(1);
	hardware_libgpiod_set_pin_table_for_test(&s_test_table);
	ASSERT(hardware_libgpiod_init(&s_test_table) == 0);
	ASSERT(hardware_gpio_mode(13, "output", errbuf, sizeof(errbuf)) == 0);
	ASSERT(hardware_libgpiod_held_count_for_test() == 1);
	ASSERT(hardware_libgpiod_release_count_for_test() == 0);
	hardware_libgpiod_shutdown();
	ASSERT(hardware_libgpiod_held_count_for_test() == 0);
	hardware_libgpiod_enable_fake_lines_for_test(0);
	hardware_libgpiod_set_pin_table_for_test(NULL);
	return 0;
}

#endif /* HAVE_LIBGPIOD */

int main(void)
{
	RUN(test_compile_smoke());
#ifdef HAVE_LIBGPIOD
	RUN(test_gpio_requires_init());
	RUN(test_sfio_rejection());
	RUN(test_gpio_validation_and_non_sfio_paths());
	RUN(test_gpio_write_holds_line_request());
	RUN(test_gpio_mode_output_holds_until_shutdown());
	RUN(test_mockup_read_write());
	RUN(test_mockup_snapshot_output_readonly());
#endif
	printf("test_hardware_libgpiod: all tests passed\n");
	return 0;
}
