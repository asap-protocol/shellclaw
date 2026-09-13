/** @file config.h. TOML path from CLI or default; env overrides file. Read-only after load. */

#ifndef SHELLCLAW_CONFIG_H
#define SHELLCLAW_CONFIG_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Opaque configuration handle. Allocated by config_load(), freed by config_free(). */
typedef struct config config_t;

/**
 * Load configuration from a TOML file.
 * Expands ~ in path. Environment variables override file values. Validates required
 * fields; on failure returns non-zero and optionally writes message to errbuf.
 *
 * @param path    Path to config file (e.g. ~/.shellclaw/config.toml).
 * @param out     On success, *out is set to allocated config_t; caller must config_free().
 * @param errbuf  Optional buffer for error message (may be NULL).
 * @param errbufsz Size of errbuf (ignored if errbuf is NULL).
 * @return 0 on success, non-zero on error.
 */
int config_load(const char *path, config_t **out, char *errbuf, size_t errbufsz);

/**
 * Free configuration. Safe to call with NULL.
 *
 * @param cfg Config to free (may be NULL).
 */
void config_free(config_t *cfg);

/* --- Getters (read-only; all return internal pointers or values) --- */

const char *config_agent_model(const config_t *c);
int config_agent_max_tokens(const config_t *c);
double config_agent_temperature(const config_t *c);
int config_agent_max_tool_iterations(const config_t *c);
int config_agent_max_context_messages(const config_t *c);
const char *config_agent_soul_path(const config_t *c);
const char *config_agent_identity_path(const config_t *c);
const char *config_agent_user_path(const config_t *c);
/** Optional [agent] fallbacks when ip-api is unavailable (degrees). Zero if unset by TOML/env. */
int config_agent_has_latitude(const config_t *c);
double config_agent_latitude(const config_t *c);
int config_agent_has_longitude(const config_t *c);
double config_agent_longitude(const config_t *c);
/** Optional ISO country code string for holiday lookup (typically two letters); may be NULL. */
const char *config_agent_country_code(const config_t *c);

const char *config_default_provider(const config_t *c);
const char *config_provider_anthropic_api_key_env(const config_t *c);
const char *config_provider_openai_api_key_env(const config_t *c);
const char *config_provider_openai_endpoint(const config_t *c);
/** Ordered fallback chain entries (provider names matching router). Defaults: anthropic, openai, local. */
int config_provider_fallback_chain_count(const config_t *c);
/** Name at index, or NULL if out of range. */
const char *config_provider_fallback_chain_entry(const config_t *c, int index);
/** OpenAI-compatible llama-server URL (chat completions path). Default http://127.0.0.1:8080/v1/chat/completions. */
const char *config_provider_local_endpoint(const config_t *c);
/** Model id sent to local server. Default tinyllama-1.1b-q4. */
const char *config_provider_local_model(const config_t *c);

int config_telegram_enabled(const config_t *c);
const char *config_telegram_token_env(const config_t *c);
int config_telegram_allowed_users_count(const config_t *c);
const char *config_telegram_allowed_user(const config_t *c, int index);

/** Discord gateway channel. Deny-by-default: empty `allowed_user_ids` rejects all users. */
int config_discord_enabled(const config_t *c);
/**
 * Env var name for the bot token. Defaults to `DISCORD_BOT_TOKEN` when unset in TOML.
 */
const char *config_discord_token_env(const config_t *c);
int config_discord_allowed_user_ids_count(const config_t *c);
/** Snowflake id string at index; NULL if out of range (same contract as Telegram allowlist). */
const char *config_discord_allowed_user_id(const config_t *c, int index);

const char *config_memory_db_path(const config_t *c);
const char *config_skills_dir(const config_t *c);
int config_workspace_only(const config_t *c);
const char *config_workspace_path(const config_t *c);
int config_shell_timeout_sec(const config_t *c);

/** Non-zero when the process sandbox (namespace isolation) is enabled. Default 0. */
int config_sandbox_enabled(const config_t *c);
/** Memory ceiling in bytes for the sandbox cgroup memory.max limit. Default 64 MiB. */
size_t config_sandbox_memory_max_bytes(const config_t *c);
/** CPU quota string for cgroups v2 "cpu.max" (e.g. "50000 100000"). NULL = unlimited. */
const char *config_sandbox_cpu_max(const config_t *c);
/** Base directory for the cgroup hierarchy. NULL = "/sys/fs/cgroup". */
const char *config_sandbox_cgroup_base(const config_t *c);

int config_gateway_enabled(const config_t *c);
const char *config_gateway_host(const config_t *c);
int config_gateway_port(const config_t *c);
int config_gateway_allow_bind_all(const config_t *c);

int config_asap_enabled(const config_t *c);
const char *config_asap_agent_urn(const config_t *c);
const char *config_asap_agent_name(const config_t *c);
/** Short agent description for ASAP manifest (required by upstream schema). */
const char *config_asap_description(const config_t *c);
/**
 * Public HTTP base URL (no trailing path). Manifest builds endpoints.asap as base + "/asap".
 */
const char *config_asap_public_base_url(const config_t *c);
/**
 * Optional per-skill description override from [asap.skill_descriptions]; NULL if unset.
 */
const char *config_asap_skill_description(const config_t *c, const char *skill_id);
const char *config_asap_registry_url(const config_t *c);
/**
 * Optional URL for the revoked-agents list (e.g. GET revoked_agents.json).
 * When NULL, the registry module may derive a default from #config_asap_registry_url.
 */
const char *config_asap_revocation_list_url(const config_t *c);
/** HTTP(S) request timeout in seconds for ASAP client calls (outbound). Default 30. */
int config_asap_client_timeout_sec(const config_t *c);
/**
 * Inbound ASAP trust list size from `[asap].trusted_senders`.
 * If zero, any sender is accepted; if non-zero, only listed URNs pass #asap_server_handle.
 */
int config_asap_trusted_senders_count(const config_t *c);
/** Trusted sender URN at index, or NULL if out of range. */
const char *config_asap_trusted_sender(const config_t *c, int index);

int config_heartbeat_enabled(const config_t *c);
int config_heartbeat_interval_minutes(const config_t *c);
const char *config_heartbeat_default_channel(const config_t *c);

const char *config_brave_api_key_env(const config_t *c);
/** Name of the env var holding the Tavily API key. Default "TAVILY_API_KEY". */
const char *config_tavily_api_key_env(const config_t *c);

/** Non-zero when the hardware tool layer is enabled. Default 1. */
int config_hardware_enabled(const config_t *c);
/**
 * Optional board override (e.g. "jetson", "rpi", "stub").
 * NULL or empty string means auto-detect at runtime.
 */
const char *config_hardware_board(const config_t *c);
/** Manifest hardware.class override; NULL uses board-specific default at manifest build time. */
const char *config_hardware_class(const config_t *c);
/** Manifest hardware.model override; NULL uses board-specific default. */
const char *config_hardware_model(const config_t *c);
/** Count of manifest hardware.io strings from [hardware].io; 0 means use board default. */
int config_hardware_io_count(const config_t *c);
/** IO capability string at index (e.g. "gpio"); NULL if out of range. */
const char *config_hardware_io_entry(const config_t *c, int index);
/** Non-zero when i2c_bus was set in TOML or SHELLCLAW_I2C_BUS; else use per-board default. */
int config_hardware_has_i2c_bus(const config_t *c);
int config_hardware_i2c_bus(const config_t *c);
/** Camera backend selector: "csi", "usb", or "auto" (default). */
const char *config_hardware_camera_type(const config_t *c);
/** Capture resolution string, e.g. "640x480" (default). */
const char *config_hardware_camera_resolution(const config_t *c);
/** JPEG quality 1–100 (default 75). */
int config_hardware_camera_quality(const config_t *c);
/** Non-zero when gpio_test_pin was set; else use per-board default (13 Jetson, 11 RPi). */
int config_hardware_has_gpio_test_pin(const config_t *c);
int config_hardware_gpio_test_pin(const config_t *c);

/** Expand ~ prefix to $HOME in path. Returns malloc'd string. Caller must free. */
char *config_expand_tilde(const char *path);

#ifdef __cplusplus
}
#endif

#endif /* SHELLCLAW_CONFIG_H */
