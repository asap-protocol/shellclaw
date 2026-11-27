/**
 * @file manifest.h
 * @brief ASAP manifest public API (re-exports builders + health).
 */
#ifndef SHELLCLAW_ASAP_MANIFEST_H
#define SHELLCLAW_ASAP_MANIFEST_H

#include "asap/manifest_build.h"
#include "asap/manifest_sign.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Return the ASAP health response as JSON, or NULL on allocation failure.
 * The caller frees the returned string.
 *
 * @param cfg             Agent configuration, or NULL for defaults.
 * @param uptime_seconds  Seconds since the gateway started.
 */
char *manifest_health_json(const config_t *cfg, double uptime_seconds);

#ifdef __cplusplus
}
#endif

#endif /* SHELLCLAW_ASAP_MANIFEST_H */
