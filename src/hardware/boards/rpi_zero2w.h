/**
 * @file rpi_zero2w.h
 * @brief 40-pin header mapping for Raspberry Pi Zero 2 W (BCM2837, gpiochip0).
 *
 * Physical pin numbers follow the standard Pi header layout; line_num is the BCM
 * GPIO offset on /dev/gpiochip0 (bcm2835-gpio). Default I2C bus = 1.
 */

#ifndef SHELLCLAW_RPI_ZERO2W_H
#define SHELLCLAW_RPI_ZERO2W_H

#include "hardware/pin_table.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RPI_HDR_PWR(pin, label)                                                        \
	{ (pin), 0u, (unsigned int)(4000u + (unsigned int)(pin)), 1, (label) }

#define RPI_HDR_SFIO(pin, line, label)                                                 \
	{ (pin), 0u, (unsigned int)(line), 1, (label) }

#define RPI_HDR_GPIO(pin, line, label)                                                 \
	{ (pin), 0u, (unsigned int)(line), 0, (label) }

static const hardware_pin_entry_t rpi_zero2w_pin_entries[] = {
	RPI_HDR_PWR(1, "3V3"),
	RPI_HDR_PWR(2, "5V"),
	RPI_HDR_SFIO(3, 2, "GPIO2_SDA1"),
	RPI_HDR_PWR(4, "5V"),
	RPI_HDR_SFIO(5, 3, "GPIO3_SCL1"),
	RPI_HDR_PWR(6, "GND"),
	RPI_HDR_GPIO(7, 4, "GPIO4"),
	RPI_HDR_SFIO(8, 14, "GPIO14_TXD0"),
	RPI_HDR_PWR(9, "GND"),
	RPI_HDR_SFIO(10, 15, "GPIO15_RXD0"),
	RPI_HDR_GPIO(11, 17, "GPIO17"),
	RPI_HDR_GPIO(12, 18, "GPIO18"),
	RPI_HDR_GPIO(13, 27, "GPIO27"),
	RPI_HDR_PWR(14, "GND"),
	RPI_HDR_GPIO(15, 22, "GPIO22"),
	RPI_HDR_GPIO(16, 23, "GPIO23"),
	RPI_HDR_PWR(17, "3V3"),
	RPI_HDR_GPIO(18, 24, "GPIO24"),
	RPI_HDR_SFIO(19, 10, "GPIO10_MOSI"),
	RPI_HDR_PWR(20, "GND"),
	RPI_HDR_SFIO(21, 9, "GPIO9_MISO"),
	RPI_HDR_GPIO(22, 25, "GPIO25"),
	RPI_HDR_SFIO(23, 11, "GPIO11_SCLK"),
	RPI_HDR_SFIO(24, 8, "GPIO8_CE0"),
	RPI_HDR_PWR(25, "GND"),
	RPI_HDR_SFIO(26, 7, "GPIO7_CE1"),
	RPI_HDR_SFIO(27, 0, "ID_SDA"),
	RPI_HDR_SFIO(28, 1, "ID_SCL"),
	RPI_HDR_GPIO(29, 5, "GPIO5"),
	RPI_HDR_PWR(30, "GND"),
	RPI_HDR_GPIO(31, 6, "GPIO6"),
	RPI_HDR_GPIO(32, 12, "GPIO12"),
	RPI_HDR_GPIO(33, 13, "GPIO13"),
	RPI_HDR_PWR(34, "GND"),
	RPI_HDR_GPIO(35, 19, "GPIO19"),
	RPI_HDR_GPIO(36, 16, "GPIO16"),
	RPI_HDR_GPIO(37, 26, "GPIO26"),
	RPI_HDR_GPIO(38, 20, "GPIO20"),
	RPI_HDR_PWR(39, "GND"),
	RPI_HDR_GPIO(40, 21, "GPIO21"),
};

static const hardware_pin_table_t rpi_zero2w_pin_table = {
	.entries = rpi_zero2w_pin_entries,
	.count = (int)(sizeof(rpi_zero2w_pin_entries) / sizeof(rpi_zero2w_pin_entries[0])),
};

#ifdef __cplusplus
}
#endif

#endif /* SHELLCLAW_RPI_ZERO2W_H */
