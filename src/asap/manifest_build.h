/**
 * @file manifest_build.h
 * @brief ASAP Manifest JSON tree builders (unsigned manifest document).
 */
#ifndef SHELLCLAW_ASAP_MANIFEST_BUILD_H
#define SHELLCLAW_ASAP_MANIFEST_BUILD_H

#include "cJSON.h"

struct config;
typedef struct config config_t;

#ifdef __cplusplus
extern "C" {
#endif

/** Build manifest object tree; caller owns returned cJSON (Delete when done). */
cJSON *manifest_build_tree(const config_t *cfg);

/** Build upstream Manifest JSON string; caller must free. */
char *manifest_build_json(const config_t *cfg);

#ifdef __cplusplus
}
#endif

#endif /* SHELLCLAW_ASAP_MANIFEST_BUILD_H */
