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
 * Return ASAP health JSON string.
 * Static string; do not free.
 */
const char *manifest_health_json(void);

#ifdef __cplusplus
}
#endif

#endif /* SHELLCLAW_ASAP_MANIFEST_H */
