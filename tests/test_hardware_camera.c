/**
 * @file test_hardware_camera.c
 * @brief hardware_camera backend tests with injectable spawn hook.
 */

#include "test_runner.h"
#include "hardware/hardware_camera.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static char s_spawn_out_path[256];
static int s_spawn_fail;
static int s_spawn_called;
static int s_spawn_bad_jpeg;
static int s_auto_temp_existed;

static int argv_has_shell_metachar(const char *s)
{
	const char *bad = ";|&$`<>\"'\n\r%";
	const char *p;
	if (!s)
		return 0;
	/* GStreamer pipeline token; passed as its own argv element, not via a shell. */
	if (strcmp(s, "!") == 0)
		return 0;
	for (p = s; *p; p++) {
		if (strchr(bad, *p) != NULL)
			return 1;
	}
	return 0;
}

static int argv_all_safe(const char *const *argv)
{
	int i;
	if (!argv)
		return 1;
	for (i = 0; argv[i] != NULL; i++) {
		if (argv_has_shell_metachar(argv[i]))
			return 0;
	}
	return 1;
}

static void spawn_find_out_path(char *const argv[])
{
	int i;

	s_spawn_out_path[0] = '\0';
	for (i = 0; argv[i] != NULL; i++) {
		const char *loc = strstr(argv[i], "location=");
		if (loc) {
			snprintf(s_spawn_out_path, sizeof(s_spawn_out_path), "%s",
				 loc + strlen("location="));
			break;
		}
		if (strcmp(argv[i], "--output") == 0 && argv[i + 1]) {
			snprintf(s_spawn_out_path, sizeof(s_spawn_out_path), "%s",
				 argv[i + 1]);
			break;
		}
		if (strcmp(argv[i], "--stream-to") == 0 && argv[i + 1]) {
			snprintf(s_spawn_out_path, sizeof(s_spawn_out_path), "%s",
				 argv[i + 1]);
			break;
		}
	}
}

static int spawn_write_output(int valid_jpeg)
{
	FILE *f;

	if (s_spawn_out_path[0] == '\0')
		return 0;
	f = fopen(s_spawn_out_path, "wb");
	ASSERT(f != NULL);
	if (valid_jpeg) {
		ASSERT(fputc(0xff, f) != EOF);
		ASSERT(fputc(0xd8, f) != EOF);
		ASSERT(fputc(0xff, f) != EOF);
		ASSERT(fputc(0xd9, f) != EOF);
	} else {
		/* Wrong magic bytes so output_jpeg_valid rejects the capture. */
		ASSERT(fputc(0xde, f) != EOF);
		ASSERT(fputc(0xad, f) != EOF);
		ASSERT(fputc(0xbe, f) != EOF);
		ASSERT(fputc(0xef, f) != EOF);
	}
	fclose(f);
	return 0;
}

static int mock_spawn(char *const argv[], char *errbuf, size_t errbufsz)
{
	const char *const *last;
	(void)errbuf;
	(void)errbufsz;
	s_spawn_called = 1;
	last = hardware_camera_last_argv_for_test();
	ASSERT(last != NULL);
	ASSERT(argv_all_safe(last));
	spawn_find_out_path(argv);
	s_auto_temp_existed = -1;
	if (strncmp(s_spawn_out_path, "/tmp/shellclaw_cam_", 19) == 0)
		s_auto_temp_existed = (access(s_spawn_out_path, F_OK) == 0) ? 1 : 0;
	if (s_spawn_fail) {
		/* Simulate a tool that created the output file before exiting non-zero. */
		RUN(spawn_write_output(1));
		if (errbuf && errbufsz > 0)
			snprintf(errbuf, errbufsz, "%s", HARDWARE_CAMERA_ERR_UNAVAILABLE);
		return -1;
	}
	RUN(spawn_write_output(!s_spawn_bad_jpeg));
	return 0;
}

static int setup_mock(void)
{
	s_spawn_fail = 0;
	s_spawn_bad_jpeg = 0;
	s_spawn_called = 0;
	s_auto_temp_existed = -1;
	s_spawn_out_path[0] = '\0';
	hardware_camera_set_spawn_for_test(mock_spawn);
	ASSERT(hardware_camera_init() == 0);
	return 0;
}

static void teardown(void)
{
	hardware_camera_set_workspace(NULL);
	hardware_camera_shutdown();
	hardware_camera_set_spawn_for_test(NULL);
}

static int test_stub_board_unavailable(void)
{
	char result[256];
	char err[128];
	RUN(setup_mock());
	ASSERT(hardware_camera_capture(BOARD_STUB, "auto", "640x480", 75, 0, 0, NULL,
				       result, sizeof(result), err, sizeof(err)) == -1);
	ASSERT(strstr(err, HARDWARE_CAMERA_ERR_UNAVAILABLE) != NULL);
	ASSERT(s_spawn_called == 0);
	teardown();
	return 0;
}

static int test_capture_requires_init(void)
{
	char result[256];
	char err[128];
	hardware_camera_shutdown();
	ASSERT(hardware_camera_capture(BOARD_JETSON_ORIN_NANO, "csi", "640x480", 75, 0,
				       0, NULL, result, sizeof(result), err,
				       sizeof(err)) == -1);
	ASSERT(strstr(err, "not initialized") != NULL);
	return 0;
}

static int test_capture_argument_validation(void)
{
	char result[256];
	char err[128];
	RUN(setup_mock());
	ASSERT(hardware_camera_capture(BOARD_JETSON_ORIN_NANO, "csi", "bad", 75, 0, 0,
				       NULL, result, sizeof(result), err,
				       sizeof(err)) == -1);
	ASSERT(strstr(err, "resolution") != NULL);
	ASSERT(hardware_camera_capture(BOARD_JETSON_ORIN_NANO, "csi", "640x480", 0, 0,
				       0, NULL, result, sizeof(result), err,
				       sizeof(err)) == -1);
	ASSERT(strstr(err, "quality") != NULL);
	ASSERT(hardware_camera_capture(BOARD_JETSON_ORIN_NANO, "csi", "640x480", 75, 9,
				       0, NULL, result, sizeof(result), err,
				       sizeof(err)) == -1);
	ASSERT(strstr(err, "sensor_id") != NULL);
	ASSERT(s_spawn_called == 0);
	teardown();
	return 0;
}

static int test_unsafe_output_path_rejected(void)
{
	char result[256];
	char err[128];
	RUN(setup_mock());
	ASSERT(hardware_camera_capture(BOARD_JETSON_ORIN_NANO, "csi", "640x480", 75, 0,
				       0, "/tmp/evil;rm -rf /", result, sizeof(result),
				       err, sizeof(err)) == -1);
	ASSERT(s_spawn_called == 0);
	teardown();
	return 0;
}

static int test_output_path_outside_workspace_rejected(void)
{
	char result[256];
	char err[128];
	char ws[128];

	ASSERT(test_runner_mkdtemp_path("shellclaw_cam_ws", ws, sizeof(ws)) == 0);
	hardware_camera_set_workspace(ws);
	RUN(setup_mock());
	ASSERT(hardware_camera_capture(BOARD_JETSON_ORIN_NANO, "csi", "640x480", 75, 0, 0,
				       "/tmp/shellclaw_escape.jpg", result, sizeof(result),
				       err, sizeof(err)) == -1);
	ASSERT(strstr(err, "workspace") != NULL);
	ASSERT(s_spawn_called == 0);
	teardown();
	rmdir(ws);
	return 0;
}

/**
 * workspace_only with empty workspace_path must fail closed (file.c parity).
 * Previously path_inside_workspace treated empty s_workspace as "allow all".
 */
static int test_empty_workspace_enforced_denies_outside(void)
{
	char result[256];
	char err[128];
	const char *outside = "/tmp/shellclaw_cam_empty_ws_escape.jpg";

	unlink(outside);
	/* Non-NULL empty string: workspace_only on, path missing/empty. */
	hardware_camera_set_workspace("");
	ASSERT(hardware_camera_output_allowed(outside) == 0);
	RUN(setup_mock());
	ASSERT(hardware_camera_capture(BOARD_JETSON_ORIN_NANO, "csi", "640x480", 75, 0, 0,
				       outside, result, sizeof(result), err,
				       sizeof(err)) == -1);
	ASSERT(strstr(err, "workspace") != NULL);
	ASSERT(s_spawn_called == 0);
	ASSERT(access(outside, F_OK) != 0);
	teardown();
	/* NULL: workspace_only off — containment disabled. */
	hardware_camera_init();
	hardware_camera_set_workspace(NULL);
	ASSERT(hardware_camera_output_allowed(outside) == 1);
	hardware_camera_shutdown();
	return 0;
}

static int test_output_path_inside_workspace_allowed(void)
{
	char result[256];
	char err[128];
	char ws[128];
	char inside[256];

	ASSERT(test_runner_mkdtemp_path("shellclaw_cam_wsok", ws, sizeof(ws)) == 0);
	snprintf(inside, sizeof(inside), "%s/shot.jpg", ws);
	hardware_camera_set_workspace(ws);
	RUN(setup_mock());
	ASSERT(hardware_camera_capture(BOARD_JETSON_ORIN_NANO, "csi", "640x480", 75, 0, 0,
				       inside, result, sizeof(result), err,
				       sizeof(err)) == 0);
	unlink(inside);
	teardown();
	rmdir(ws);
	return 0;
}

static int test_output_path_traversal_rejected(void)
{
	char result[256];
	char err[128];
	RUN(setup_mock());
	ASSERT(hardware_camera_capture(BOARD_JETSON_ORIN_NANO, "csi", "640x480", 75, 0,
				       0, "/tmp/../etc/passwd.jpg", result,
				       sizeof(result), err, sizeof(err)) == -1);
	ASSERT(strstr(err, "unsafe output path") != NULL);
	ASSERT(s_spawn_called == 0);
	teardown();
	return 0;
}

static int test_resolution_injection_rejected(void)
{
	char result[256];
	char err[128];
	RUN(setup_mock());
	ASSERT(hardware_camera_capture(BOARD_JETSON_ORIN_NANO, "csi", "640x480;rm -rf /",
				       75, 0, 0, NULL, result, sizeof(result), err,
				       sizeof(err)) == -1);
	ASSERT(strstr(err, "resolution") != NULL);
	ASSERT(s_spawn_called == 0);
	teardown();
	return 0;
}

static int test_camera_type_injection_rejected(void)
{
	char result[256];
	char err[128];
	RUN(setup_mock());
	ASSERT(hardware_camera_capture(BOARD_JETSON_ORIN_NANO, "csi|sh", "640x480", 75, 0,
				       0, NULL, result, sizeof(result), err,
				       sizeof(err)) == -1);
	ASSERT(strstr(err, "camera_type") != NULL);
	ASSERT(s_spawn_called == 0);
	teardown();
	return 0;
}

/** sensor_id is int 0-3; argv must use numeric sensor-id= only (no shell tokens). */
static int test_sensor_id_injection_rejected(void)
{
	char result[256];
	char err[128];
	const char *const *argv;
	int i;
	int found_sensor = 0;
	RUN(setup_mock());
	ASSERT(hardware_camera_capture(BOARD_JETSON_ORIN_NANO, "csi", "640x480", 75, 2,
				       0, NULL, result, sizeof(result), err,
				       sizeof(err)) == 0);
	argv = hardware_camera_last_argv_for_test();
	ASSERT(argv != NULL);
	for (i = 0; argv[i] != NULL; i++) {
		if (strncmp(argv[i], "sensor-id=", 10) == 0) {
			found_sensor = 1;
			ASSERT(strcmp(argv[i], "sensor-id=2") == 0);
			ASSERT(argv_has_shell_metachar(argv[i]) == 0);
		}
	}
	ASSERT(found_sensor == 1);
	ASSERT(hardware_camera_capture(BOARD_JETSON_ORIN_NANO, "csi", "640x480", 75, 4,
				       0, NULL, result, sizeof(result), err,
				       sizeof(err)) == -1);
	ASSERT(strstr(err, "sensor_id") != NULL);
	teardown();
	return 0;
}

static int test_no_shell_invocation(void)
{
	const char *const *argv;
	int i;
	char result[256];
	char err[128];
	RUN(setup_mock());
	ASSERT(hardware_camera_capture(BOARD_JETSON_ORIN_NANO, "csi", "640x480", 75, 0,
				       0, NULL, result, sizeof(result), err,
				       sizeof(err)) == 0);
	argv = hardware_camera_last_argv_for_test();
	ASSERT(argv != NULL);
	for (i = 0; argv[i] != NULL; i++) {
		ASSERT(strcmp(argv[i], "sh") != 0);
		ASSERT(strcmp(argv[i], "/bin/sh") != 0);
		ASSERT(strstr(argv[i], " -c ") == NULL);
	}
	teardown();
	return 0;
}

static int test_jetson_csi_argv_no_shell_metacharacters(void)
{
	char result[256];
	char err[128];
	const char *const *argv;
	int i;
	int found_src = 0;
	int found_buffers = 0;
	RUN(setup_mock());
	ASSERT(hardware_camera_capture(BOARD_JETSON_ORIN_NANO, "csi", "640x480", 75, 1,
				       0, NULL, result, sizeof(result), err,
				       sizeof(err)) == 0);
	ASSERT(s_spawn_called == 1);
	argv = hardware_camera_last_argv_for_test();
	ASSERT(argv != NULL);
	ASSERT(argv_all_safe(argv));
	ASSERT(strcmp(argv[0], "gst-launch-1.0") == 0);
	for (i = 0; argv[i] != NULL; i++) {
		if (strcmp(argv[i], "nvarguscamerasrc") == 0)
			found_src = 1;
		if (strcmp(argv[i], "num-buffers=4") == 0)
			found_buffers = 1;
	}
	ASSERT(found_src == 1);
	ASSERT(found_buffers == 1);
	teardown();
	return 0;
}

static int test_spawn_failure_unavailable(void)
{
	char result[256];
	char err[128];
	RUN(setup_mock());
	s_spawn_fail = 1;
	ASSERT(hardware_camera_capture(BOARD_JETSON_ORIN_NANO, "csi", "640x480", 75, 0,
				       0, NULL, result, sizeof(result), err,
				       sizeof(err)) == -1);
	ASSERT(strstr(err, HARDWARE_CAMERA_ERR_UNAVAILABLE) != NULL);
	teardown();
	return 0;
}

static int test_usb_backend_argv(void)
{
	char result[256];
	char err[128];
	const char *const *argv;
	RUN(setup_mock());
	ASSERT(hardware_camera_capture(BOARD_JETSON_ORIN_NANO, "usb", "320x240", 75, 0,
				       2, NULL, result, sizeof(result), err,
				       sizeof(err)) == 0);
	argv = hardware_camera_last_argv_for_test();
	ASSERT(argv != NULL);
	ASSERT(strcmp(argv[0], "v4l2-ctl") == 0);
	ASSERT(strstr(argv[2], "/dev/video2") != NULL);
	teardown();
	return 0;
}

static int test_rpi_csi_argv(void)
{
	char result[256];
	char err[128];
	const char *const *argv;
	int i;
	int found_width = 0;
	int found_height = 0;
	RUN(setup_mock());
	ASSERT(hardware_camera_capture(BOARD_RPI_ZERO2W, "csi", "640x480", 75, 0, 0, NULL,
				       result, sizeof(result), err, sizeof(err)) == 0);
	ASSERT(s_spawn_called == 1);
	argv = hardware_camera_last_argv_for_test();
	ASSERT(argv != NULL);
	ASSERT(strcmp(argv[0], "libcamera-still") == 0);
	for (i = 0; argv[i] != NULL; i++) {
		if (strcmp(argv[i], "--width") == 0 && argv[i + 1] &&
		    strcmp(argv[i + 1], "640") == 0)
			found_width = 1;
		if (strcmp(argv[i], "--height") == 0 && argv[i + 1] &&
		    strcmp(argv[i + 1], "480") == 0)
			found_height = 1;
	}
	ASSERT(found_width == 1);
	ASSERT(found_height == 1);
	teardown();
	return 0;
}

static int argv_contains_after(const char *const *argv, const char *marker,
			       const char *expected)
{
	int i;
	int seen_marker = 0;

	for (i = 0; argv[i] != NULL; i++) {
		if (seen_marker && strcmp(argv[i], expected) == 0)
			return 1;
		if (strcmp(argv[i], marker) == 0)
			seen_marker = 1;
	}
	return 0;
}

static int argv_has_prefix(const char *const *argv, const char *prefix)
{
	int i;

	for (i = 0; argv[i] != NULL; i++) {
		if (strncmp(argv[i], prefix, strlen(prefix)) == 0)
			return 1;
	}
	return 0;
}

static int argv_contains(const char *const *argv, const char *token)
{
	int i;

	for (i = 0; argv[i] != NULL; i++) {
		if (strcmp(argv[i], token) == 0)
			return 1;
	}
	return 0;
}

static int test_jetson_csi_quality_in_argv(void)
{
	char result[256];
	char err[128];
	const char *const *argv;
	RUN(setup_mock());
	ASSERT(hardware_camera_capture(BOARD_JETSON_ORIN_NANO, "csi", "640x480", 80, 0,
				       0, NULL, result, sizeof(result), err,
				       sizeof(err)) == 0);
	argv = hardware_camera_last_argv_for_test();
	ASSERT(argv != NULL);
	ASSERT(argv_contains_after(argv, "nvjpegenc", "quality=80"));
	ASSERT(hardware_camera_capture(BOARD_JETSON_ORIN_NANO, "csi", "640x480", 100, 0,
				       0, NULL, result, sizeof(result), err,
				       sizeof(err)) == 0);
	argv = hardware_camera_last_argv_for_test();
	ASSERT(argv != NULL);
	ASSERT(argv_contains_after(argv, "nvjpegenc", "quality=100"));
	teardown();
	return 0;
}

static int test_rpi_csi_quality_in_argv(void)
{
	char result[256];
	char err[128];
	const char *const *argv;
	RUN(setup_mock());
	ASSERT(hardware_camera_capture(BOARD_RPI_ZERO2W, "csi", "640x480", 42, 0, 0,
				       NULL, result, sizeof(result), err,
				       sizeof(err)) == 0);
	argv = hardware_camera_last_argv_for_test();
	ASSERT(argv != NULL);
	ASSERT(argv_contains(argv, "--quality"));
	ASSERT(argv_contains_after(argv, "--quality", "42"));
	teardown();
	return 0;
}

static int test_usb_quality_not_in_argv(void)
{
	char result[256];
	char err[128];
	const char *const *argv;
	RUN(setup_mock());
	ASSERT(hardware_camera_capture(BOARD_JETSON_ORIN_NANO, "usb", "320x240", 55, 0,
				       2, NULL, result, sizeof(result), err,
				       sizeof(err)) == 0);
	argv = hardware_camera_last_argv_for_test();
	ASSERT(argv != NULL);
	/* UVC MJPG quality is firmware-controlled; no quality flag may reach v4l2-ctl. */
	ASSERT(argv_has_prefix(argv, "quality=") == 0);
	ASSERT(argv_contains(argv, "--quality") == 0);
	teardown();
	return 0;
}

static int test_auto_temp_keeps_exclusive_inode(void)
{
	char result[256];
	char err[128];

	RUN(setup_mock());
	ASSERT(hardware_camera_capture(BOARD_JETSON_ORIN_NANO, "csi", "640x480", 75, 0,
				       0, NULL, result, sizeof(result), err,
				       sizeof(err)) == 0);
	ASSERT(s_auto_temp_existed == 1);
	ASSERT(strstr(result, ".jpg") == NULL);
	unlink(result);
	teardown();
	return 0;
}

static int test_temp_jpeg_unlinked_on_spawn_failure(void)
{
	char result[256];
	char err[128];
	char leaked_path[256];
	RUN(setup_mock());
	s_spawn_fail = 1;
	ASSERT(hardware_camera_capture(BOARD_JETSON_ORIN_NANO, "csi", "640x480", 75, 0,
				       0, NULL, result, sizeof(result), err,
				       sizeof(err)) == -1);
	ASSERT(strstr(err, HARDWARE_CAMERA_ERR_UNAVAILABLE) != NULL);
	ASSERT(s_spawn_out_path[0] != '\0');
	snprintf(leaked_path, sizeof(leaked_path), "%s", s_spawn_out_path);
	ASSERT(access(leaked_path, F_OK) != 0);
	teardown();
	return 0;
}

static int test_temp_jpeg_unlinked_on_invalid_jpeg(void)
{
	char result[256];
	char err[128];
	char leaked_path[256];
	RUN(setup_mock());
	s_spawn_bad_jpeg = 1;
	ASSERT(hardware_camera_capture(BOARD_JETSON_ORIN_NANO, "csi", "640x480", 75, 0,
				       0, NULL, result, sizeof(result), err,
				       sizeof(err)) == -1);
	ASSERT(strstr(err, HARDWARE_CAMERA_ERR_UNAVAILABLE) != NULL);
	ASSERT(s_spawn_out_path[0] != '\0');
	snprintf(leaked_path, sizeof(leaked_path), "%s", s_spawn_out_path);
	ASSERT(access(leaked_path, F_OK) != 0);
	teardown();
	return 0;
}

static int test_caller_supplied_output_not_unlinked_on_error(void)
{
	char result[256];
	char err[128];
	char caller_path[128];
	FILE *f;
	RUN(setup_mock());
	ASSERT(test_runner_mkstemp_path("shellclaw_cam_caller", caller_path,
					 sizeof(caller_path)) == 0);
	f = fopen(caller_path, "wb");
	ASSERT(f != NULL);
	ASSERT(fputc('x', f) != EOF);
	fclose(f);
	s_spawn_fail = 1;
	ASSERT(hardware_camera_capture(BOARD_JETSON_ORIN_NANO, "csi", "640x480", 75, 0,
				       0, caller_path, result, sizeof(result), err,
				       sizeof(err)) == -1);
	/* Caller-owned path must survive the module's internal failure. */
	ASSERT(access(caller_path, F_OK) == 0);
	unlink(caller_path);
	teardown();
	return 0;
}

static int test_default_spawn_times_out(void)
{
	char *argv[] = { "/bin/sleep", "5", NULL };
	char err[128];

	hardware_camera_set_spawn_timeout_ms_for_test(80);
	ASSERT(hardware_camera_default_spawn_for_test(argv, err, sizeof(err)) != 0);
	ASSERT(strstr(err, "timed out") != NULL);
	hardware_camera_set_spawn_timeout_ms_for_test(0);
	return 0;
}

int main(void)
{
	RUN(test_capture_requires_init());
	RUN(test_stub_board_unavailable());
	RUN(test_capture_argument_validation());
	RUN(test_unsafe_output_path_rejected());
	RUN(test_output_path_outside_workspace_rejected());
	RUN(test_empty_workspace_enforced_denies_outside());
	RUN(test_output_path_inside_workspace_allowed());
	RUN(test_output_path_traversal_rejected());
	RUN(test_resolution_injection_rejected());
	RUN(test_camera_type_injection_rejected());
	RUN(test_sensor_id_injection_rejected());
	RUN(test_no_shell_invocation());
	RUN(test_jetson_csi_argv_no_shell_metacharacters());
	RUN(test_spawn_failure_unavailable());
	RUN(test_usb_backend_argv());
	RUN(test_rpi_csi_argv());
	RUN(test_jetson_csi_quality_in_argv());
	RUN(test_rpi_csi_quality_in_argv());
	RUN(test_usb_quality_not_in_argv());
	RUN(test_auto_temp_keeps_exclusive_inode());
	RUN(test_temp_jpeg_unlinked_on_spawn_failure());
	RUN(test_temp_jpeg_unlinked_on_invalid_jpeg());
	RUN(test_caller_supplied_output_not_unlinked_on_error());
	RUN(test_default_spawn_times_out());
	printf("All hardware_camera tests passed.\n");
	return 0;
}
