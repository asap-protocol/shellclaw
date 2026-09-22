/**
 * @file jetson_orin_nano.h
 * @brief 40-pin header mapping for Jetson Orin Nano / Orin Nano Super (JetPack 6.x).
 *
 * Line offsets are the gpiochip0 line numbers from the JetsonHacks J12 pinout
 * (tegra234-gpio). Re-verify on hardware with: gpioinfo gpiochip0
 * See: https://jetsonhacks.com/nvidia-jetson-orin-nano-gpio-header-pinout/
 */

#ifndef SHELLCLAW_JETSON_ORIN_NANO_H
#define SHELLCLAW_JETSON_ORIN_NANO_H

#include "hardware/pin_table.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Power/ground rows: unique sentinel line, SFIO blocks GPIO tool access. */
#define JETSON_HDR_PWR(pin, label)                                                     \
	{ (pin), 0u, (unsigned int)(4000u + (unsigned int)(pin)), 1, (label) }

#define JETSON_HDR_SFIO(pin, line, label)                                              \
	{ (pin), 0u, (unsigned int)(line), 1, (label) }

#define JETSON_HDR_GPIO(pin, line, label)                                              \
	{ (pin), 0u, (unsigned int)(line), 0, (label) }

static const hardware_pin_entry_t jetson_orin_nano_pin_entries[] = {
	JETSON_HDR_PWR(1, "3V3"),
	JETSON_HDR_PWR(2, "5V"),
	JETSON_HDR_SFIO(3, 2, "I2C1_SDA"),
	JETSON_HDR_PWR(4, "5V"),
	JETSON_HDR_SFIO(5, 3, "I2C1_SCL"),
	JETSON_HDR_PWR(6, "GND"),
	JETSON_HDR_GPIO(7, 144, "GPIO09"),
	JETSON_HDR_SFIO(8, 108, "UART1_TX"),
	JETSON_HDR_PWR(9, "GND"),
	JETSON_HDR_SFIO(10, 109, "UART1_RX"),
	JETSON_HDR_SFIO(11, 112, "UART1_RTS"),
	JETSON_HDR_SFIO(12, 50, "I2S0_SCLK"),
	JETSON_HDR_SFIO(13, 122, "SPI1_SCK"),
	JETSON_HDR_PWR(14, "GND"),
	JETSON_HDR_GPIO(15, 85, "GPIO12"),
	JETSON_HDR_SFIO(16, 126, "SPI1_CS1"),
	JETSON_HDR_PWR(17, "3V3"),
	JETSON_HDR_SFIO(18, 125, "SPI1_CS0"),
	JETSON_HDR_SFIO(19, 135, "SPI0_MOSI"),
	JETSON_HDR_PWR(20, "GND"),
	JETSON_HDR_SFIO(21, 134, "SPI0_MISO"),
	JETSON_HDR_SFIO(22, 123, "SPI1_MISO"),
	JETSON_HDR_SFIO(23, 133, "SPI0_SCK"),
	JETSON_HDR_SFIO(24, 136, "SPI0_CS0"),
	JETSON_HDR_PWR(25, "GND"),
	JETSON_HDR_SFIO(26, 137, "SPI0_CS1"),
	JETSON_HDR_SFIO(27, 140, "I2C0_SDA"),
	JETSON_HDR_SFIO(28, 141, "I2C0_SCL"),
	JETSON_HDR_GPIO(29, 105, "GPIO01"),
	JETSON_HDR_PWR(30, "GND"),
	JETSON_HDR_GPIO(31, 106, "GPIO11"),
	JETSON_HDR_GPIO(32, 41, "GPIO07"),
	JETSON_HDR_GPIO(33, 43, "GPIO13"),
	JETSON_HDR_PWR(34, "GND"),
	JETSON_HDR_SFIO(35, 53, "I2S0_FS"),
	JETSON_HDR_SFIO(36, 113, "UART1_CTS"),
	JETSON_HDR_SFIO(37, 124, "SPI1_MOSI"),
	JETSON_HDR_SFIO(38, 52, "I2S0_SDIN"),
	JETSON_HDR_PWR(39, "GND"),
	JETSON_HDR_SFIO(40, 51, "I2S0_SDOUT"),
};

static const hardware_pin_table_t jetson_orin_nano_pin_table = {
	.entries = jetson_orin_nano_pin_entries,
	.count = (int)(sizeof(jetson_orin_nano_pin_entries) /
		      sizeof(jetson_orin_nano_pin_entries[0])),
};

#ifdef __cplusplus
}
#endif

#endif /* SHELLCLAW_JETSON_ORIN_NANO_H */
