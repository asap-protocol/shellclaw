/**
 * @file hardware_tegrastats.h
 * @brief Parse JetPack 6.x tegrastats lines and probe llama-server for the GPU API.
 */

#ifndef SHELLCLAW_HARDWARE_TEGRASTATS_H
#define SHELLCLAW_HARDWARE_TEGRASTATS_H

#include "cJSON.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Parsed fields from one tegrastats stdout line (JetPack 6.2.x / Orin). */
typedef struct hardware_tegrastats_parsed {
	unsigned int ram_used_mb;
	unsigned int ram_total_mb;
	unsigned int gpu_usage_percent;
	unsigned int gpu_freq_mhz;
	float gpu_temp_c;
	int has_gpu_temp;
	char power_mode[64];
} hardware_tegrastats_parsed_t;

/**
 * Parse a single tegrastats line (RAM, GR3D_FREQ, optional gpu@ temp).
 * Pinned against JetPack 6.2.x: GR3D_FREQ X%@[Y1,Y2] and RAM A/BMB.
 *
 * @return 0 when RAM and GR3D_FREQ were found, -1 on failure.
 */
int hardware_tegrastats_parse_line(const char *line, hardware_tegrastats_parsed_t *out);

/**
 * Run tegrastats once and read one output line into @p linebuf.
 * @return 0 on success, -1 on failure (message in @p errbuf).
 */
int hardware_tegrastats_collect_line(char *linebuf, size_t linebufsz, char *errbuf,
				     size_t errbufsz);

/** Read current NV power mode via nvpmodel -q (empty string on failure). */
void hardware_tegrastats_read_power_mode(char *buf, size_t bufsz);

/** Non-zero when a process named llama-server is running. */
int hardware_llama_server_running(void);

/**
 * Fill @p root with Jetson GPU JSON (available true + stats + llama_server).
 * On failure returns -1 and does not modify @p root.
 */
int hardware_jetson_gpu_json_fill(cJSON *root, char *errbuf, size_t errbufsz);

/** Test hook: replace tegrastats collect (NULL restores default popen). */
typedef int (*hardware_tegrastats_collect_fn)(char *linebuf, size_t linebufsz,
					      char *errbuf, size_t errbufsz);
void hardware_tegrastats_set_collect_for_test(hardware_tegrastats_collect_fn fn);

/** Test hook: force power mode string (NULL restores nvpmodel). */
void hardware_tegrastats_set_power_mode_for_test(const char *mode);

/** Test hook: -1 = real pgrep, 0/1 = forced llama-server state. */
void hardware_tegrastats_set_llama_running_for_test(int forced);

#ifdef __cplusplus
}
#endif

#endif /* SHELLCLAW_HARDWARE_TEGRASTATS_H */
