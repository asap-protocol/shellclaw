/**
 * @file hardware_tools_internal.h
 * @brief Shared helpers for GPIO, I2C, and camera tool executors.
 */

#ifndef SHELLCLAW_TOOLS_HARDWARE_INTERNAL_H
#define SHELLCLAW_TOOLS_HARDWARE_INTERNAL_H

#include "core/config.h"
#include "cJSON.h"
#include <stddef.h>
#include <stdint.h>

extern const config_t *g_hw_cfg;

#define HW_ERR_DISABLED "{\"error\":\"hardware disabled in config\"}"
#define HW_ERR_GPIO     "{\"error\":\"GPIO not available (libgpiod or pin table missing)\"}"
#define HW_ERR_I2C      "{\"error\":\"I2C backend not initialized\"}"
#define HW_ERR_CAMERA   "{\"error\":\"camera backend not initialized\"}"
#define HW_I2C_BUS_MIN 0
#define HW_I2C_BUS_MAX 255

int hw_tools_enabled(void);

int hw_tools_parse_root(const char *args_json, cJSON **root_out, char *result_buf,
			size_t max_len);
int hw_tools_require_int(cJSON *root, const char *key, int *out, char *result_buf,
			 size_t max_len);
void hw_tools_json_error(char *result_buf, size_t max_len, const char *msg);
int hw_tools_parse_byte_array(cJSON *root, const char *key, uint8_t *out, size_t max_len,
			      size_t *len_out, char *result_buf, size_t result_max);
int hw_tools_validate_gpio_pin(int pin, char *result_buf, size_t max_len);
int hw_tools_validate_i2c_addr(int addr, char *result_buf, size_t max_len);
int hw_tools_resolve_i2c_bus(cJSON *root, int *bus_out, char *result_buf, size_t max_len);
int hw_tools_gpio_ready(void);

#endif /* SHELLCLAW_TOOLS_HARDWARE_INTERNAL_H */
