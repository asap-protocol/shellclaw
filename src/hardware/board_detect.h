/**
 * @file board_detect.h
 * @brief Runtime board identification from device-tree compatible strings.
 */

#ifndef SHELLCLAW_BOARD_DETECT_H
#define SHELLCLAW_BOARD_DETECT_H

#ifdef __cplusplus
extern "C" {
#endif

/** Supported board identifiers for backend selection. */
typedef enum board_id {
	BOARD_UNKNOWN = 0,
	BOARD_JETSON_ORIN_NANO,
	BOARD_RPI_ZERO2W,
	BOARD_STUB
} board_id_t;

/**
 * Map a board override string to #board_id_t.
 *
 * @param s Value such as "jetson", "rpi", or "stub"; NULL/empty → #BOARD_UNKNOWN.
 * @return Matching board id, or #BOARD_UNKNOWN when unrecognized.
 */
board_id_t board_id_from_string(const char *s);

/**
 * Detect the active board.
 * Honors SHELLCLAW_BOARD env override (jetson, rpi, stub) before reading
 * /proc/device-tree/compatible (or the test override path). Invalid env values
 * fall back to device-tree detection.
 *
 * @return Detected board id.
 */
board_id_t board_detect(void);

/**
 * Stable string id for scripts and logging (e.g. "jetson_orin_nano").
 *
 * @param id Board id from #board_detect.
 * @return Static string; never NULL.
 */
const char *board_name(board_id_t id);

/**
 * Override compatible file path for unit tests. Pass NULL to restore default.
 *
 * @param path Path to a NUL-separated compatible blob, or NULL for default.
 */
void board_detect_set_path_for_test(const char *path);

#ifdef __cplusplus
}
#endif

#endif /* SHELLCLAW_BOARD_DETECT_H */
