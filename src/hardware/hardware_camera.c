/**
 * @file hardware_camera.c
 * @brief Camera capture via fixed-argv CLI tools (no shell interpolation).
 */
#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "hardware/hardware_camera.h"
#include <errno.h>
#include <libgen.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define CAPS_BUF_SZ 128
#define ARG_BUF_SZ 64
#define MIN_JPEG_BYTES 2
#define HARDWARE_CAMERA_SPAWN_TIMEOUT_MS 15000

typedef enum camera_cli_kind {
	CAMERA_CLI_NONE = 0,
	CAMERA_CLI_JETSON_CSI,
	CAMERA_CLI_USB_UVC,
	CAMERA_CLI_RPI_CSI
} camera_cli_kind_t;

static int s_camera_ready;
static char s_workspace[PATH_MAX];
static hardware_camera_spawn_fn s_test_spawn;
static char *s_last_argv[HARDWARE_CAMERA_ARGV_MAX];
static char s_last_argv_storage[HARDWARE_CAMERA_ARGV_MAX][ARG_BUF_SZ];
static int s_last_argv_count;
static int s_spawn_timeout_ms = HARDWARE_CAMERA_SPAWN_TIMEOUT_MS;

/* cppcheck-suppress constParameter */
static void record_argv(char *const argv[])
{
	int i = 0;
	s_last_argv_count = 0;
	while (argv && argv[i] && i < HARDWARE_CAMERA_ARGV_MAX) {
		snprintf(s_last_argv_storage[i], ARG_BUF_SZ, "%s", argv[i]);
		s_last_argv[i] = s_last_argv_storage[i];
		i++;
	}
	s_last_argv_count = i;
}

const char *const *hardware_camera_last_argv_for_test(void)
{
	if (s_last_argv_count <= 0)
		return NULL;
	return (const char *const *)s_last_argv;
}

void hardware_camera_set_spawn_for_test(hardware_camera_spawn_fn fn)
{
	s_test_spawn = fn;
}

static void set_err(char *errbuf, size_t errbufsz, const char *msg)
{
	if (errbuf && errbufsz > 0)
		snprintf(errbuf, errbufsz, "%s", msg);
}

static int path_chars_safe(const char *path)
{
	const char *p;
	if (!path || path[0] == '\0')
		return 0;
	if (strstr(path, "..") != NULL)
		return 0;
	for (p = path; *p; p++) {
		unsigned char c = (unsigned char)*p;
		if (c == ';' || c == '|' || c == '&' || c == '$' || c == '`' ||
		    c == '!' || c == '<' || c == '>' || c == '"' || c == '\'' ||
		    c == '\n' || c == '\r' || c == ' ' || c == '\t')
			return 0;
		if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
		    (c >= '0' && c <= '9') || c == '/' || c == '_' || c == '-' ||
		    c == '.')
			continue;
		return 0;
	}
	return 1;
}

static int resolved_under_workspace(const char *resolved, const char *ws_resolved)
{
	size_t ws_len;

	ws_len = strlen(ws_resolved);
	if (strncmp(resolved, ws_resolved, ws_len) != 0)
		return 0;
	if (resolved[ws_len] != '\0' && resolved[ws_len] != '/')
		return 0;
	return 1;
}

static int path_inside_workspace(const char *path)
{
	char ws_resolved[PATH_MAX];
	char resolved[PATH_MAX];
	char path_copy[PATH_MAX];

	if (s_workspace[0] == '\0' || !path || path[0] == '\0')
		return 1;
	if (realpath(s_workspace, ws_resolved) == NULL)
		return 0;
	if (realpath(path, resolved) != NULL)
		return resolved_under_workspace(resolved, ws_resolved);
	snprintf(path_copy, sizeof(path_copy), "%s", path);
	for (;;) {
		char *dir = dirname(path_copy);
		if (!dir || dir[0] == '\0')
			break;
		if (realpath(dir, resolved) != NULL)
			return resolved_under_workspace(resolved, ws_resolved);
		if (strcmp(dir, ".") == 0 || strcmp(dir, "/") == 0)
			break;
		snprintf(path_copy, sizeof(path_copy), "%s", dir);
	}
	return 0;
}

static int parse_resolution(const char *resolution, unsigned int *w, unsigned int *h,
			    char *errbuf, size_t errbufsz)
{
	unsigned int width = 0;
	unsigned int height = 0;
	int n;
	int consumed = 0;
	if (!resolution || resolution[0] == '\0') {
		set_err(errbuf, errbufsz, "camera: missing resolution");
		return -1;
	}
	n = sscanf(resolution, "%ux%u%n", &width, &height, &consumed);
	if (n != 2 || width == 0 || height == 0 || width > 4096u || height > 4096u ||
	    resolution[consumed] != '\0') {
		set_err(errbuf, errbufsz, "camera: invalid resolution (expected WxH)");
		return -1;
	}
	*w = width;
	*h = height;
	return 0;
}

static int camera_type_allowed(const char *camera_type)
{
	if (!camera_type || camera_type[0] == '\0')
		return 1;
	return strcmp(camera_type, "auto") == 0 || strcmp(camera_type, "csi") == 0 ||
	       strcmp(camera_type, "usb") == 0;
}

static int camera_type_is_usb(const char *camera_type)
{
	return camera_type && strcmp(camera_type, "usb") == 0;
}

static int camera_type_is_csi(const char *camera_type)
{
	return camera_type && strcmp(camera_type, "csi") == 0;
}

static camera_cli_kind_t resolve_cli(board_id_t board, const char *camera_type)
{
	if (board == BOARD_STUB || board == BOARD_UNKNOWN)
		return CAMERA_CLI_NONE;
	if (camera_type_is_usb(camera_type))
		return CAMERA_CLI_USB_UVC;
	if (board == BOARD_JETSON_ORIN_NANO) {
		if (camera_type_is_csi(camera_type) ||
		    !camera_type || strcmp(camera_type, "auto") == 0)
			return CAMERA_CLI_JETSON_CSI;
		return CAMERA_CLI_NONE;
	}
	if (board == BOARD_RPI_ZERO2W) {
		if (camera_type_is_csi(camera_type) ||
		    !camera_type || strcmp(camera_type, "auto") == 0)
			return CAMERA_CLI_RPI_CSI;
		return CAMERA_CLI_NONE;
	}
	return CAMERA_CLI_NONE;
}

static const char *program_for_kind(camera_cli_kind_t kind)
{
	switch (kind) {
	case CAMERA_CLI_JETSON_CSI:
		return "gst-launch-1.0";
	case CAMERA_CLI_USB_UVC:
		return "v4l2-ctl";
	case CAMERA_CLI_RPI_CSI:
		return "libcamera-still";
	default:
		return NULL;
	}
}

static int tool_executable(const char *prog)
{
	char path[128];
	if (!prog)
		return 0;
	if (access(prog, X_OK) == 0)
		return 1;
	if (snprintf(path, sizeof(path), "/usr/bin/%s", prog) >= (int)sizeof(path))
		return 0;
	return access(path, X_OK) == 0;
}

static int wait_child_timeout(pid_t pid, char *errbuf, size_t errbufsz)
{
	int elapsed = 0;
	int status = 0;
	const int slice_ms = 20;
	struct timespec ts;

	while (elapsed < s_spawn_timeout_ms) {
		pid_t r = waitpid(pid, &status, WNOHANG);
		if (r == pid) {
			if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
				set_err(errbuf, errbufsz, HARDWARE_CAMERA_ERR_UNAVAILABLE);
				return -1;
			}
			return 0;
		}
		if (r < 0) {
			set_err(errbuf, errbufsz, HARDWARE_CAMERA_ERR_UNAVAILABLE);
			return -1;
		}
		ts.tv_sec = 0;
		ts.tv_nsec = (long)slice_ms * 1000000L;
		nanosleep(&ts, NULL);
		elapsed += slice_ms;
	}
	kill(pid, SIGKILL);
	(void)waitpid(pid, &status, 0);
	set_err(errbuf, errbufsz, "camera: capture timed out");
	return -1;
}

static int default_spawn(char *const argv[], char *errbuf, size_t errbufsz)
{
	pid_t pid;
	if (!argv || !argv[0]) {
		set_err(errbuf, errbufsz, HARDWARE_CAMERA_ERR_UNAVAILABLE);
		return -1;
	}
	pid = fork();
	if (pid < 0) {
		set_err(errbuf, errbufsz, HARDWARE_CAMERA_ERR_UNAVAILABLE);
		return -1;
	}
	if (pid == 0) {
		execvp(argv[0], argv);
		_exit(127);
	}
	return wait_child_timeout(pid, errbuf, errbufsz);
}

void hardware_camera_set_spawn_timeout_ms_for_test(int ms)
{
	s_spawn_timeout_ms = (ms > 0) ? ms : HARDWARE_CAMERA_SPAWN_TIMEOUT_MS;
}

int hardware_camera_default_spawn_for_test(char *const argv[], char *errbuf, size_t errbufsz)
{
	return default_spawn(argv, errbuf, errbufsz);
}

static int run_cli(char *const argv[], char *errbuf, size_t errbufsz)
{
	hardware_camera_spawn_fn spawn = s_test_spawn ? s_test_spawn : default_spawn;
	record_argv(argv);
	return spawn(argv, errbuf, errbufsz);
}

static int make_temp_output(char *path, size_t pathsz)
{
	char tmpl[] = "/tmp/shellclaw_cam_XXXXXX";
	int fd;
	fd = mkstemp(tmpl);
	if (fd < 0)
		return -1;
	close(fd);
	if (strlen(tmpl) + 1 > pathsz) {
		unlink(tmpl);
		return -1;
	}
	/* Keep the exclusive inode; capture CLIs accept any path and JPEG is
	 * checked by magic bytes. Unlinking then writing ${tmpl}.jpg races a
	 * symlink on the sibling name. */
	snprintf(path, pathsz, "%s", tmpl);
	return 0;
}

static int output_jpeg_valid(const char *path)
{
	struct stat st;
	unsigned char hdr[2];
	FILE *f;
	if (!path || stat(path, &st) != 0 || st.st_size < MIN_JPEG_BYTES)
		return 0;
	f = fopen(path, "rb");
	if (!f)
		return 0;
	if (fread(hdr, 1, sizeof(hdr), f) != sizeof(hdr)) {
		fclose(f);
		return 0;
	}
	fclose(f);
	return hdr[0] == 0xff && hdr[1] == 0xd8;
}

static int build_jetson_csi_argv(char **argv, char *arg0, char *arg1, char *arg2,
				 char *arg_q, size_t arg_bufsz, unsigned int width,
				 unsigned int height, int sensor_id, int quality,
				 const char *out_path)
{
	int n;
	n = snprintf(arg0, arg_bufsz, "sensor-id=%d", sensor_id);
	if (n < 0 || (size_t)n >= arg_bufsz)
		return -1;
	n = snprintf(arg1, arg_bufsz,
		     "video/x-raw(memory:NVMM),width=%u,height=%u,format=NV12", width,
		     height);
	if (n < 0 || (size_t)n >= arg_bufsz)
		return -1;
	n = snprintf(arg2, arg_bufsz, "location=%s", out_path);
	if (n < 0 || (size_t)n >= arg_bufsz)
		return -1;
	/* nvjpegenc exposes a 0-100 quality property; emit it as its own argv token. */
	n = snprintf(arg_q, arg_bufsz, "quality=%d", quality);
	if (n < 0 || (size_t)n >= arg_bufsz)
		return -1;
	argv[0] = "gst-launch-1.0";
	argv[1] = "-e";
	argv[2] = "nvarguscamerasrc";
	argv[3] = arg0;
	argv[4] = "num-buffers=4";
	argv[5] = "!";
	argv[6] = arg1;
	argv[7] = "!";
	argv[8] = "nvjpegenc";
	argv[9] = arg_q;
	argv[10] = "!";
	argv[11] = "filesink";
	argv[12] = arg2;
	argv[13] = NULL;
	return 0;
}

static int build_usb_argv(char **argv, char *arg0, char *arg1, size_t arg_bufsz,
			  unsigned int width, unsigned int height, int video_index,
			  int quality, const char *out_path)
{
	int n;
	/* UVC MJPG quality is set by camera firmware; v4l2-ctl has no quality flag, so
	 * quality is accepted for builder-signature uniformity but intentionally ignored. */
	(void)quality;
	n = snprintf(arg0, arg_bufsz, "/dev/video%d", video_index);
	if (n < 0 || (size_t)n >= arg_bufsz)
		return -1;
	n = snprintf(arg1, arg_bufsz, "width=%u,height=%u,pixelformat=MJPG", width,
		     height);
	if (n < 0 || (size_t)n >= arg_bufsz)
		return -1;
	argv[0] = "v4l2-ctl";
	argv[1] = "--device";
	argv[2] = arg0;
	argv[3] = "--set-fmt-video";
	argv[4] = arg1;
	argv[5] = "--stream-mmap";
	argv[6] = "--stream-count=1";
	argv[7] = "--stream-to";
	argv[8] = (char *)out_path;
	argv[9] = NULL;
	return 0;
}

static int build_rpi_csi_argv(char **argv, char *wbuf, size_t wbufsz, char *hbuf, size_t hbufsz,
			      char *qbuf, size_t qbufsz, unsigned int width, unsigned int height,
			      int quality, const char *out_path)
{
	snprintf(wbuf, wbufsz, "%u", width);
	snprintf(hbuf, hbufsz, "%u", height);
	snprintf(qbuf, qbufsz, "%d", quality);
	argv[0] = "libcamera-still";
	argv[1] = "--output";
	argv[2] = (char *)out_path;
	argv[3] = "--width";
	argv[4] = wbuf;
	argv[5] = "--height";
	argv[6] = hbuf;
	argv[7] = "--quality";
	argv[8] = qbuf;
	argv[9] = "--nopreview";
	argv[10] = "--timeout";
	argv[11] = "1000";
	argv[12] = NULL;
	return 0;
}

int hardware_camera_init(void)
{
	s_camera_ready = 1;
	return 0;
}

void hardware_camera_set_workspace(const char *workspace)
{
	if (!workspace || workspace[0] == '\0') {
		s_workspace[0] = '\0';
		return;
	}
	snprintf(s_workspace, sizeof(s_workspace), "%s", workspace);
}

int hardware_camera_output_allowed(const char *path)
{
	if (!path || path[0] == '\0')
		return 1;
	return path_inside_workspace(path);
}

void hardware_camera_shutdown(void)
{
	s_test_spawn = NULL;
	s_camera_ready = 0;
	s_workspace[0] = '\0';
	s_spawn_timeout_ms = HARDWARE_CAMERA_SPAWN_TIMEOUT_MS;
	s_last_argv_count = 0;
	memset(s_last_argv, 0, sizeof(s_last_argv));
}

int hardware_camera_is_available(void)
{
	return s_camera_ready ? 1 : 0;
}

static int validate_capture_inputs(board_id_t board, const char *camera_type,
				   const char *resolution, int quality, int sensor_id,
				   int video_index, char *result_path, size_t result_pathsz,
				   unsigned int *width_out, unsigned int *height_out,
				   camera_cli_kind_t *kind_out, char *errbuf, size_t errbufsz)
{
	unsigned int width = 0;
	unsigned int height = 0;
	camera_cli_kind_t kind;
	const char *prog;

	if (!s_camera_ready) {
		set_err(errbuf, errbufsz, "camera: backend not initialized");
		return -1;
	}
	if (!result_path || result_pathsz == 0) {
		set_err(errbuf, errbufsz, "camera: result_path is NULL");
		return -1;
	}
	if (quality < 1 || quality > 100) {
		set_err(errbuf, errbufsz, "camera: quality must be 1-100");
		return -1;
	}
	if (sensor_id < 0 || sensor_id > 3) {
		set_err(errbuf, errbufsz, "camera: sensor_id must be 0-3");
		return -1;
	}
	if (video_index < 0 || video_index > 99) {
		set_err(errbuf, errbufsz, "camera: video_index must be 0-99");
		return -1;
	}
	if (!camera_type_allowed(camera_type)) {
		set_err(errbuf, errbufsz, "camera: invalid camera_type");
		return -1;
	}
	if (parse_resolution(resolution, &width, &height, errbuf, errbufsz) != 0)
		return -1;
	kind = resolve_cli(board, camera_type);
	if (kind == CAMERA_CLI_NONE) {
		set_err(errbuf, errbufsz, HARDWARE_CAMERA_ERR_UNAVAILABLE);
		return -1;
	}
	prog = program_for_kind(kind);
	if (!s_test_spawn && !tool_executable(prog)) {
		set_err(errbuf, errbufsz, HARDWARE_CAMERA_ERR_UNAVAILABLE);
		return -1;
	}
	*width_out = width;
	*height_out = height;
	*kind_out = kind;
	return 0;
}

static int prepare_capture_output_path(const char *output_path, char *out_path,
				       size_t out_pathsz, int *auto_output, char *errbuf,
				       size_t errbufsz)
{
	if (output_path && output_path[0] != '\0') {
		if (!path_chars_safe(output_path)) {
			set_err(errbuf, errbufsz, "camera: unsafe output path");
			return -1;
		}
		if (!path_inside_workspace(output_path)) {
			set_err(errbuf, errbufsz, "camera: path outside workspace");
			return -1;
		}
		snprintf(out_path, out_pathsz, "%s", output_path);
		*auto_output = 0;
		return 0;
	}
	if (make_temp_output(out_path, out_pathsz) != 0) {
		set_err(errbuf, errbufsz, HARDWARE_CAMERA_ERR_UNAVAILABLE);
		return -1;
	}
	/* Module owns auto-generated temp files and must unlink them on internal error. */
	*auto_output = 1;
	return 0;
}

static int build_argv_and_run(camera_cli_kind_t kind, unsigned int width,
			      unsigned int height, int sensor_id, int video_index,
			      int quality, int auto_output, const char *out_path,
			      char *result_path, size_t result_pathsz, char *errbuf,
			      size_t errbufsz)
{
	char *argv[HARDWARE_CAMERA_ARGV_MAX];
	char arg0[ARG_BUF_SZ];
	char arg1[CAPS_BUF_SZ];
	char arg2[ARG_BUF_SZ];
	char arg_q[ARG_BUF_SZ];
	char rpi_wbuf[16];
	char rpi_hbuf[16];
	char rpi_qbuf[16];

	memset(argv, 0, sizeof(argv));
	if (kind == CAMERA_CLI_JETSON_CSI) {
		if (build_jetson_csi_argv(argv, arg0, arg1, arg2, arg_q, ARG_BUF_SZ, width,
					  height, sensor_id, quality, out_path) != 0) {
			set_err(errbuf, errbufsz, HARDWARE_CAMERA_ERR_UNAVAILABLE);
			return -1;
		}
	} else if (kind == CAMERA_CLI_USB_UVC) {
		if (build_usb_argv(argv, arg0, arg1, ARG_BUF_SZ, width, height, video_index,
				   quality, out_path) != 0) {
			set_err(errbuf, errbufsz, HARDWARE_CAMERA_ERR_UNAVAILABLE);
			return -1;
		}
	} else if (build_rpi_csi_argv(argv, rpi_wbuf, sizeof(rpi_wbuf), rpi_hbuf,
				       sizeof(rpi_hbuf), rpi_qbuf, sizeof(rpi_qbuf), width,
				       height, quality, out_path) != 0) {
		set_err(errbuf, errbufsz, HARDWARE_CAMERA_ERR_UNAVAILABLE);
		return -1;
	}
	if (run_cli(argv, errbuf, errbufsz) != 0) {
		if (auto_output)
			unlink(out_path);
		return -1;
	}
	if (!output_jpeg_valid(out_path)) {
		set_err(errbuf, errbufsz, HARDWARE_CAMERA_ERR_UNAVAILABLE);
		if (auto_output)
			unlink(out_path);
		return -1;
	}
	snprintf(result_path, result_pathsz, "%s", out_path);
	return 0;
}

int hardware_camera_capture(board_id_t board, const char *camera_type,
			    const char *resolution, int quality, int sensor_id,
			    int video_index, const char *output_path, char *result_path,
			    size_t result_pathsz, char *errbuf, size_t errbufsz)
{
	char out_path[256];
	unsigned int width = 0;
	unsigned int height = 0;
	camera_cli_kind_t kind = CAMERA_CLI_NONE;
	int auto_output = 0;

	if (validate_capture_inputs(board, camera_type, resolution, quality, sensor_id,
				    video_index, result_path, result_pathsz, &width, &height,
				    &kind, errbuf, errbufsz) != 0)
		return -1;
	if (prepare_capture_output_path(output_path, out_path, sizeof(out_path),
					&auto_output, errbuf, errbufsz) != 0)
		return -1;
	return build_argv_and_run(kind, width, height, sensor_id, video_index, quality,
				  auto_output, out_path, result_path, result_pathsz, errbuf,
				  errbufsz);
}
