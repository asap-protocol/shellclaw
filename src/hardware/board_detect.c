/**
 * @file board_detect.c
 * @brief Read /proc/device-tree/compatible and classify the host board.
 */
#define _POSIX_C_SOURCE 200809L

#include "board_detect.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEFAULT_COMPATIBLE_PATH "/proc/device-tree/compatible"
#define ENV_SHELLCLAW_BOARD "SHELLCLAW_BOARD"

static const char *s_compatible_path = DEFAULT_COMPATIBLE_PATH;

void board_detect_set_path_for_test(const char *path)
{
	s_compatible_path = path ? path : DEFAULT_COMPATIBLE_PATH;
}

board_id_t board_id_from_string(const char *s)
{
	if (!s || s[0] == '\0')
		return BOARD_UNKNOWN;
	if (strcmp(s, "jetson") == 0)
		return BOARD_JETSON_ORIN_NANO;
	if (strcmp(s, "rpi") == 0)
		return BOARD_RPI_ZERO2W;
	if (strcmp(s, "stub") == 0)
		return BOARD_STUB;
	return BOARD_UNKNOWN;
}

static board_id_t classify_compatible(const char *data, size_t len)
{
	size_t offset = 0;
	while (offset < len) {
		const char *entry = data + offset;
		size_t entry_len = strnlen(entry, len - offset);
		if (strncmp(entry, "nvidia,p3768", 12) == 0 ||
		    strncmp(entry, "tegra234", 8) == 0)
			return BOARD_JETSON_ORIN_NANO;
		if (strncmp(entry, "raspberrypi,model-zero-2-w", 26) == 0)
			return BOARD_RPI_ZERO2W;
		if (entry_len == 0)
			break;
		offset += entry_len + 1;
	}
	return BOARD_UNKNOWN;
}

static board_id_t detect_from_compatible_file(const char *path)
{
	FILE *fp = NULL;
	char *buf = NULL;
	size_t cap = 0;
	size_t len = 0;
	board_id_t id = BOARD_UNKNOWN;
	fp = fopen(path, "rb");
	if (!fp)
		return BOARD_UNKNOWN;
	while (1) {
		size_t nread;

		if (len + 256 > cap) {
			char *grown = NULL;
			size_t new_cap = cap == 0 ? 256 : cap * 2;
			grown = realloc(buf, new_cap);
			if (!grown) {
				free(buf);
				fclose(fp);
				return BOARD_UNKNOWN;
			}
			buf = grown;
			cap = new_cap;
		}
		nread = fread(buf + len, 1, cap - len, fp);
		len += nread;
		if (nread == 0)
			break;
	}
	fclose(fp);
	if (len == 0) {
		free(buf);
		return BOARD_UNKNOWN;
	}
	id = classify_compatible(buf, len);
	free(buf);
	return id;
}

board_id_t board_detect(void)
{
	const char *env = getenv(ENV_SHELLCLAW_BOARD);

	if (env != NULL && env[0] != '\0') {
		board_id_t env_id = board_id_from_string(env);
		if (env_id != BOARD_UNKNOWN)
			return env_id;
	}
	return detect_from_compatible_file(s_compatible_path);
}

const char *board_name(board_id_t id)
{
	switch (id) {
	case BOARD_JETSON_ORIN_NANO:
		return "jetson_orin_nano";
	case BOARD_RPI_ZERO2W:
		return "rpi_zero2w";
	case BOARD_STUB:
		return "stub";
	case BOARD_UNKNOWN:
	default:
		return "unknown";
	}
}
