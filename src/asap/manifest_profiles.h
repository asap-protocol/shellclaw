/**
 * @file manifest_profiles.h
 * @brief Board profiles for ASAP manifest capabilities.hardware / inference.
 */
#ifndef SHELLCLAW_ASAP_MANIFEST_PROFILES_H
#define SHELLCLAW_ASAP_MANIFEST_PROFILES_H

#include "hardware/board_detect.h"

struct config;
typedef struct config config_t;

#ifdef __cplusplus
extern "C" {
#endif

typedef struct manifest_board_profile {
	const char *class_name;
	const char *model_name;
	const char *const *io;
	int io_count;
	const char *const *modes;
	int mode_count;
	const char *local_model_id;
	const char *local_quantization;
} manifest_board_profile_t;

board_id_t manifest_resolve_board(const config_t *cfg);
const manifest_board_profile_t *manifest_board_profile(board_id_t board);

#ifdef __cplusplus
}
#endif

#endif /* SHELLCLAW_ASAP_MANIFEST_PROFILES_H */
