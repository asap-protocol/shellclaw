/**
 * @file hardware_camera.h
 * @brief External CLI camera capture (GStreamer / v4l2 / libcamera) with test hooks.
 */

#ifndef SHELLCLAW_HARDWARE_CAMERA_H
#define SHELLCLAW_HARDWARE_CAMERA_H

#include "hardware/board_detect.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HARDWARE_CAMERA_ARGV_MAX 24
#define HARDWARE_CAMERA_ERR_UNAVAILABLE "camera not available"

/** Spawn hook used by unit tests (NULL restores fork/execvp). */
typedef int (*hardware_camera_spawn_fn)(char *const argv[], char *errbuf, size_t errbufsz);

int hardware_camera_init(void);
void hardware_camera_shutdown(void);
int hardware_camera_is_available(void);

/**
 * Capture a still frame to a JPEG file.
 *
 * Base64 encoding is deferred to Phase 5 slice 02; on success @p result_path receives
 * the output JPEG path (caller buffer or auto-generated temp file). When @p output_path
 * is NULL the module owns the temp file: it unlinks it on internal failure (spawn or
 * JPEG-validation error) and leaves it in place on success for the caller to consume.
 *
 * @param board Active board id (from #board_detect).
 * @param camera_type Config value: "csi", "usb", or "auto".
 * @param resolution "WxH" (digits only).
 * @param quality JPEG quality 1–100 (applied to nvjpegenc/libcamera; ignored on USB UVC MJPG).
 * @param sensor_id CSI sensor index (Jetson nvarguscamerasrc).
 * @param video_index USB /dev/videoN index.
 * @param output_path Optional output path; NULL selects a secure temp file.
 * @param result_path Buffer for the JPEG path written on success.
 * @param result_pathsz Size of @p result_path.
 * @param errbuf Error message on failure (e.g. #HARDWARE_CAMERA_ERR_UNAVAILABLE).
 * @param errbufsz Size of @p errbuf.
 * @return 0 on success, -1 on error.
 */
int hardware_camera_capture(board_id_t board, const char *camera_type,
			    const char *resolution, int quality, int sensor_id,
			    int video_index, const char *output_path, char *result_path,
			    size_t result_pathsz, char *errbuf, size_t errbufsz);

/**
 * Bind the file-tool workspace root used for caller-supplied capture paths.
 * NULL disables containment (workspace_only off). Non-NULL enables enforcement;
 * an empty string fails closed (deny caller paths) — same as write_file when
 * workspace_path is missing/empty under workspace_only.
 *
 * Example: hardware_camera_set_workspace("") denies /tmp/out.jpg; NULL allows it.
 */
void hardware_camera_set_workspace(const char *workspace);

/**
 * Return 1 if @p path may be used as a caller-supplied capture output.
 * Auto temp (NULL/empty) is always allowed. When enforcement is on, the path
 * must resolve under the bound root (same policy as write_file).
 */
int hardware_camera_output_allowed(const char *path);

void hardware_camera_set_spawn_timeout_ms_for_test(int ms);
int hardware_camera_default_spawn_for_test(char *const argv[], char *errbuf, size_t errbufsz);

/** Override spawn for unit tests (NULL restores default). */
void hardware_camera_set_spawn_for_test(hardware_camera_spawn_fn fn);

/** Last argv passed to spawn (test introspection); NULL if none yet. */
const char *const *hardware_camera_last_argv_for_test(void);

#ifdef __cplusplus
}
#endif

#endif /* SHELLCLAW_HARDWARE_CAMERA_H */
