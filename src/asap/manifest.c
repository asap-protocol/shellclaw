/**
 * @file manifest.c
 * @brief ASAP health JSON for well-known discovery.
 */
#define _POSIX_C_SOURCE 200809L

#include "asap/manifest.h"

#include "core/config.h"
#include "core/version.h"
#include "cJSON.h"

char *manifest_health_json(const config_t *cfg, double uptime_seconds)
{
	cJSON *health = cJSON_CreateObject();
	char *json = NULL;

	if (health &&
	    cJSON_AddStringToObject(health, "status", "healthy") &&
	    cJSON_AddStringToObject(health, "agent_id", config_asap_agent_urn(cfg)) &&
	    cJSON_AddStringToObject(health, "version", SHELLCLAW_RELEASE_VERSION) &&
	    cJSON_AddStringToObject(health, "asap_version", ASAP_DISCOVERY_VERSION) &&
	    cJSON_AddNumberToObject(health, "uptime_seconds", uptime_seconds))
		json = cJSON_PrintUnformatted(health);
	cJSON_Delete(health);
	return json;
}
