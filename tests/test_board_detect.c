/**
 * @file test_board_detect.c
 * @brief Unit tests for board_detect: compatible parsing and env override.
 */

#include "test_runner.h"
#include "hardware/board_detect.h"
#include <stdio.h>
#include <string.h>

static int write_compatible_file(const char *path, const char *first, const char *second)
{
	FILE *f = fopen(path, "wb");
	size_t first_len;
	size_t second_len;
	ASSERT(f);
	first_len = strlen(first);
	second_len = second ? strlen(second) : 0;
	ASSERT(fwrite(first, 1, first_len + 1, f) == first_len + 1);
	if (second != NULL)
		ASSERT(fwrite(second, 1, second_len + 1, f) == second_len + 1);
	fclose(f);
	return 0;
}

static int test_jetson_compatible(void)
{
	char path[128];
	ASSERT(test_runner_mkstemp_path("shellclaw_board_jetson", path, sizeof(path)) == 0);
	ASSERT(write_compatible_file(path, "nvidia,p3768-0000-super", "nvidia,tegra234") == 0);
	unsetenv("SHELLCLAW_BOARD");
	board_detect_set_path_for_test(path);
	ASSERT(board_detect() == BOARD_JETSON_ORIN_NANO);
	ASSERT(strcmp(board_name(BOARD_JETSON_ORIN_NANO), "jetson_orin_nano") == 0);
	board_detect_set_path_for_test(NULL);
	remove(path);
	return 0;
}

static int test_rpi_compatible(void)
{
	char path[128];
	ASSERT(test_runner_mkstemp_path("shellclaw_board_rpi", path, sizeof(path)) == 0);
	ASSERT(write_compatible_file(path, "raspberrypi,model-zero-2-w", NULL) == 0);
	unsetenv("SHELLCLAW_BOARD");
	board_detect_set_path_for_test(path);
	ASSERT(board_detect() == BOARD_RPI_ZERO2W);
	ASSERT(strcmp(board_name(BOARD_RPI_ZERO2W), "rpi_zero2w") == 0);
	board_detect_set_path_for_test(NULL);
	remove(path);
	return 0;
}

static int test_unknown_compatible(void)
{
	char path[128];
	ASSERT(test_runner_mkstemp_path("shellclaw_board_unknown", path, sizeof(path)) == 0);
	ASSERT(write_compatible_file(path, "vendor,generic-board", NULL) == 0);
	unsetenv("SHELLCLAW_BOARD");
	board_detect_set_path_for_test(path);
	ASSERT(board_detect() == BOARD_UNKNOWN);
	ASSERT(strcmp(board_name(BOARD_UNKNOWN), "unknown") == 0);
	board_detect_set_path_for_test(NULL);
	remove(path);
	return 0;
}

static int test_env_override_jetson(void)
{
	char path[128];
	ASSERT(test_runner_mkstemp_path("shellclaw_board_env", path, sizeof(path)) == 0);
	ASSERT(write_compatible_file(path, "vendor,generic-board", NULL) == 0);
	board_detect_set_path_for_test(path);
	setenv("SHELLCLAW_BOARD", "jetson", 1);
	ASSERT(board_detect() == BOARD_JETSON_ORIN_NANO);
	unsetenv("SHELLCLAW_BOARD");
	board_detect_set_path_for_test(NULL);
	remove(path);
	return 0;
}

static int test_env_override_rpi(void)
{
	char path[128];
	ASSERT(test_runner_mkstemp_path("shellclaw_board_env", path, sizeof(path)) == 0);
	ASSERT(write_compatible_file(path, "vendor,generic-board", NULL) == 0);
	board_detect_set_path_for_test(path);
	setenv("SHELLCLAW_BOARD", "rpi", 1);
	ASSERT(board_detect() == BOARD_RPI_ZERO2W);
	unsetenv("SHELLCLAW_BOARD");
	board_detect_set_path_for_test(NULL);
	remove(path);
	return 0;
}

static int test_env_override_stub(void)
{
	char path[128];
	ASSERT(test_runner_mkstemp_path("shellclaw_board_env", path, sizeof(path)) == 0);
	ASSERT(write_compatible_file(path, "nvidia,p3768", NULL) == 0);
	board_detect_set_path_for_test(path);
	setenv("SHELLCLAW_BOARD", "stub", 1);
	ASSERT(board_detect() == BOARD_STUB);
	ASSERT(strcmp(board_name(BOARD_STUB), "stub") == 0);
	unsetenv("SHELLCLAW_BOARD");
	board_detect_set_path_for_test(NULL);
	remove(path);
	return 0;
}

static int test_invalid_env_falls_back_to_compatible(void)
{
	char path[128];
	ASSERT(test_runner_mkstemp_path("shellclaw_board_bad_env", path, sizeof(path)) == 0);
	ASSERT(write_compatible_file(path, "raspberrypi,model-zero-2-w", NULL) == 0);
	board_detect_set_path_for_test(path);
	setenv("SHELLCLAW_BOARD", "not-a-board", 1);
	ASSERT(board_detect() == BOARD_RPI_ZERO2W);
	unsetenv("SHELLCLAW_BOARD");
	board_detect_set_path_for_test(NULL);
	remove(path);
	return 0;
}

int main(void)
{
	RUN(test_jetson_compatible());
	RUN(test_rpi_compatible());
	RUN(test_unknown_compatible());
	RUN(test_env_override_jetson());
	RUN(test_env_override_rpi());
	RUN(test_env_override_stub());
	RUN(test_invalid_env_falls_back_to_compatible());
	printf("test_board_detect: all tests passed\n");
	return 0;
}
