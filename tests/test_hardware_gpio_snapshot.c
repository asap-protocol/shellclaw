/**
 * @file test_hardware_gpio_snapshot.c
 * @brief Unit tests for 40-pin GPIO snapshot JSON builder.
 */
#define _POSIX_C_SOURCE 200809L

#include "hardware/hardware_gpio_snapshot.h"
#include "hardware/hardware.h"
#include "core/config.h"
#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ASSERT(c)                                                                          \
	do {                                                                               \
		if (!(c)) {                                                                \
			fprintf(stderr, "FAIL: %s:%d %s\n", __FILE__, __LINE__, #c);       \
			return 1;                                                          \
		}                                                                          \
	} while (0)

static int write_toml(const char *path, const char *hardware_section)
{
	FILE *f = fopen(path, "w");
	if (!f)
		return -1;
	fprintf(f, "[agent]\nmodel = \"test\"\n\n[hardware]\n%s", hardware_section);
	fclose(f);
	return 0;
}

static int load_cfg(const char *path, config_t **cfg_out)
{
	char errbuf[256];
	if (config_load(path, cfg_out, errbuf, sizeof(errbuf)) != 0)
		return -1;
	return 0;
}

static cJSON *pin_at(cJSON *pins, int physical)
{
	int i;
	for (i = 0; i < cJSON_GetArraySize(pins); i++) {
		cJSON *obj = cJSON_GetArrayItem(pins, i);
		cJSON *pin = cJSON_GetObjectItemCaseSensitive(obj, "pin");
		if (cJSON_IsNumber(pin) && pin->valueint == physical)
			return obj;
	}
	return NULL;
}

static int test_jetson_snapshot_shape(void)
{
	const char *path = "/tmp/shellclaw_test_gpio_snap_jetson.toml";
	config_t *cfg = NULL;
	cJSON *pins = NULL;
	cJSON *obj;

	ASSERT(write_toml(path, "enabled = true\nboard = \"jetson\"\n") == 0);
	ASSERT(load_cfg(path, &cfg) == 0);
	ASSERT(hardware_init(cfg) == 0);
	pins = cJSON_CreateArray();
	ASSERT(pins != NULL);
	ASSERT(hardware_gpio_snapshot_fill(pins, NULL, 0) == 0);
	ASSERT(cJSON_GetArraySize(pins) == 40);
	obj = pin_at(pins, 3);
	ASSERT(obj != NULL);
	ASSERT(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(obj, "sfio")));
	ASSERT(strcmp(cJSON_GetObjectItemCaseSensitive(obj, "mode")->valuestring,
		      "sfio") == 0);
	ASSERT(cJSON_IsNull(cJSON_GetObjectItemCaseSensitive(obj, "state")));
	obj = pin_at(pins, 33);
	ASSERT(obj != NULL);
	ASSERT(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(obj, "sfio")));
	cJSON_Delete(pins);
	config_free(cfg);
	return 0;
}

static int test_stub_board_no_table(void)
{
	const char *path = "/tmp/shellclaw_test_gpio_snap_stub.toml";
	config_t *cfg = NULL;
	cJSON *pins = NULL;

	ASSERT(write_toml(path, "enabled = false\n") == 0);
	ASSERT(load_cfg(path, &cfg) == 0);
	ASSERT(hardware_init(cfg) == 0);
	pins = cJSON_CreateArray();
	ASSERT(pins != NULL);
	ASSERT(hardware_gpio_snapshot_fill(pins, NULL, 0) != 0);
	cJSON_Delete(pins);
	config_free(cfg);
	return 0;
}

int main(void)
{
	int failures = 0;
	if (test_jetson_snapshot_shape() != 0)
		failures++;
	if (test_stub_board_no_table() != 0)
		failures++;
	if (failures == 0) {
		printf("test_hardware_gpio_snapshot: all tests passed\n");
		return 0;
	}
	fprintf(stderr, "test_hardware_gpio_snapshot: %d test(s) failed\n", failures);
	return 1;
}
