/**
 * @file manifest_profiles.c
 * @brief Board profile tables for ASAP manifest capabilities.
 */
#define _POSIX_C_SOURCE 200809L

#include "asap/manifest_profiles.h"
#include "core/config.h"
#include "hardware/board_detect.h"

static const char *const DEFAULT_IO_GPIO_I2C[] = { "gpio", "i2c" };
static const char *const JETSON_MODES[] = { "cloud", "local_cuda" };
static const char *const RPI_MODES[] = { "cloud", "local_cpu" };
static const char *const STUB_MODES[] = { "cloud", "local_cpu" };

board_id_t manifest_resolve_board(const config_t *cfg)
{
	board_id_t from_cfg;

	from_cfg = board_id_from_string(config_hardware_board(cfg));
	if (from_cfg != BOARD_UNKNOWN)
		return from_cfg;
	from_cfg = board_detect();
	if (from_cfg != BOARD_UNKNOWN)
		return from_cfg;
	return BOARD_STUB;
}

const manifest_board_profile_t *manifest_board_profile(board_id_t board)
{
	static const manifest_board_profile_t jetson = {
		.class_name = "edge_accelerator",
		.model_name = "jetson_orin_nano_super_8gb",
		.io = DEFAULT_IO_GPIO_I2C,
		.io_count = 2,
		.modes = JETSON_MODES,
		.mode_count = 2,
		.local_model_id = "Phi-3-mini-4k-instruct-Q4_K_M",
		.local_quantization = "Q4_K_M",
	};
	static const manifest_board_profile_t rpi = {
		.class_name = "sbc",
		.model_name = "raspberry_pi_zero_2_w",
		.io = DEFAULT_IO_GPIO_I2C,
		.io_count = 2,
		.modes = RPI_MODES,
		.mode_count = 2,
		.local_model_id = "tinyllama-1.1b-chat-Q4_K_M",
		.local_quantization = "Q4_K_M",
	};
	static const manifest_board_profile_t stub = {
		.class_name = "sbc",
		.model_name = "stub",
		.io = NULL,
		.io_count = 0,
		.modes = STUB_MODES,
		.mode_count = 2,
		.local_model_id = "tinyllama-1.1b-chat-Q4_K_M",
		.local_quantization = "Q4_K_M",
	};

	switch (board) {
	case BOARD_JETSON_ORIN_NANO:
		return &jetson;
	case BOARD_RPI_ZERO2W:
		return &rpi;
	case BOARD_STUB:
	case BOARD_UNKNOWN:
	default:
		return &stub;
	}
}
