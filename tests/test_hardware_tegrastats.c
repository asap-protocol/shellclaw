/**
 * @file test_hardware_tegrastats.c
 * @brief Unit tests for tegrastats line parser (JetPack 6.2.x sample).
 */
#define _POSIX_C_SOURCE 200809L

#include "hardware/hardware_tegrastats.h"
#include "cJSON.h"
#include <stdio.h>
#include <string.h>

#define ASSERT(c)                                                                          \
	do {                                                                               \
		if (!(c)) {                                                                \
			fprintf(stderr, "FAIL: %s:%d %s\n", __FILE__, __LINE__, #c);       \
			return 1;                                                          \
		}                                                                          \
	} while (0)

/* Captured-style line for JetPack 6.x Orin (GR3D_FREQ X%@[Y1,Y2], RAM, gpu@). */
static const char SAMPLE_JP6_LINE[] =
	"05-23-2026 12:00:00 RAM 2048/7620MB (lfb 512x4MB) SWAP 0/3810MB (cached 0MB) "
	"CPU [2%@1510,0%@729,0%@729,0%@729,0%@729,0%@729] EMC_FREQ 0%@2133 "
	"GR3D_FREQ 12%@[1020,1020] NVENC off NVDEC off gpu@42.5C tj@46.0C";

static int test_parse_jp6_line(void)
{
	hardware_tegrastats_parsed_t p;

	ASSERT(hardware_tegrastats_parse_line(SAMPLE_JP6_LINE, &p) == 0);
	ASSERT(p.ram_used_mb == 2048u);
	ASSERT(p.ram_total_mb == 7620u);
	ASSERT(p.gpu_usage_percent == 12u);
	ASSERT(p.gpu_freq_mhz == 1020u);
	ASSERT(p.has_gpu_temp == 1);
	ASSERT(p.gpu_temp_c > 42.0f && p.gpu_temp_c < 43.0f);
	return 0;
}

static int test_parse_legacy_gr3d_single_freq(void)
{
	const char *line =
		"RAM 100/8000MB (lfb 1x4MB) CPU [0%@729] GR3D_FREQ 5%@114 gpu@40.0C";
	hardware_tegrastats_parsed_t p;

	ASSERT(hardware_tegrastats_parse_line(line, &p) == 0);
	ASSERT(p.gpu_usage_percent == 5u);
	ASSERT(p.gpu_freq_mhz == 114u);
	return 0;
}

static int collect_sample(char *linebuf, size_t linebufsz, char *errbuf, size_t errbufsz)
{
	(void)errbuf;
	(void)errbufsz;
	snprintf(linebuf, linebufsz, "%s", SAMPLE_JP6_LINE);
	return 0;
}

static int collect_fail(char *linebuf, size_t linebufsz, char *errbuf, size_t errbufsz)
{
	(void)linebuf;
	(void)linebufsz;
	if (errbuf && errbufsz > 0)
		snprintf(errbuf, errbufsz, "tegrastats: forced fail");
	return -1;
}

static int test_json_fill_failure_untouched_root(void)
{
	cJSON *root;
	char err[128];

	hardware_tegrastats_set_collect_for_test(collect_fail);
	root = cJSON_CreateObject();
	ASSERT(root != NULL);
	ASSERT(hardware_jetson_gpu_json_fill(root, err, sizeof(err)) != 0);
	ASSERT(cJSON_GetObjectItemCaseSensitive(root, "available") == NULL);
	ASSERT(cJSON_GetObjectItemCaseSensitive(root, "gpu_usage") == NULL);
	cJSON_Delete(root);
	hardware_tegrastats_set_collect_for_test(NULL);
	return 0;
}

static int test_json_fill_with_hooks(void)
{
	cJSON *root;
	char err[128];

	hardware_tegrastats_set_collect_for_test(collect_sample);
	hardware_tegrastats_set_power_mode_for_test("MAXN");
	hardware_tegrastats_set_llama_running_for_test(1);
	root = cJSON_CreateObject();
	ASSERT(root != NULL);
	ASSERT(hardware_jetson_gpu_json_fill(root, err, sizeof(err)) == 0);
	ASSERT(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "available")));
	ASSERT(cJSON_GetObjectItemCaseSensitive(root, "gpu_usage")->valuedouble == 12.0);
	ASSERT(strcmp(cJSON_GetObjectItemCaseSensitive(root, "power_mode")->valuestring,
		      "MAXN") == 0);
	{
		cJSON *llama = cJSON_GetObjectItemCaseSensitive(root, "llama_server");
		ASSERT(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(llama, "running")));
	}
	cJSON_Delete(root);
	hardware_tegrastats_set_collect_for_test(NULL);
	hardware_tegrastats_set_power_mode_for_test(NULL);
	hardware_tegrastats_set_llama_running_for_test(-1);
	return 0;
}

int main(void)
{
	int failures = 0;

	if (test_parse_jp6_line() != 0)
		failures++;
	if (test_parse_legacy_gr3d_single_freq() != 0)
		failures++;
	if (test_json_fill_with_hooks() != 0)
		failures++;
	if (test_json_fill_failure_untouched_root() != 0)
		failures++;
	if (failures == 0) {
		printf("test_hardware_tegrastats: all tests passed\n");
		return 0;
	}
	fprintf(stderr, "test_hardware_tegrastats: %d test(s) failed\n", failures);
	return 1;
}
