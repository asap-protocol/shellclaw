/**
 * @file manifest.c
 * @brief ASAP health JSON for well-known discovery.
 */
#define _POSIX_C_SOURCE 200809L

#include "asap/manifest.h"

static const char *HEALTH_JSON = "{\"status\":\"ok\"}";

const char *manifest_health_json(void)
{
	return HEALTH_JSON;
}
