/**
 * @file config_patch.h
 * @brief Patch on-disk TOML from dashboard JSON updates.
 *
 * Example: config_patch_dashboard_json(path, "{\"model\":\"x\"}", &toml, &len, err, sizeof(err));
 */

#ifndef SHELLCLAW_CONFIG_PATCH_H
#define SHELLCLAW_CONFIG_PATCH_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Merge dashboard JSON fields into an existing config.toml.
 *
 * Accepts JSON objects with optional keys: model, max_tokens, temperature,
 * gateway_host, gateway_port. Unmentioned keys are left unchanged on disk.
 *
 * @param config_path Path to config.toml.
 * @param json_body   NUL-terminated JSON object body.
 * @param out_toml    On success, allocated patched TOML (caller frees).
 * @param out_len     Length of patched TOML.
 * @param errbuf      Optional error buffer.
 * @param errbufsz    Size of errbuf.
 * @return 0 on success, non-zero on error.
 */
int config_patch_dashboard_json(const char *config_path, const char *json_body,
                                char **out_toml, size_t *out_len, char *errbuf,
                                size_t errbufsz);

#ifdef __cplusplus
}
#endif

#endif /* SHELLCLAW_CONFIG_PATCH_H */
