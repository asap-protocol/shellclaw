/**
 * @file reload_channel_stub.c
 * @brief No-op channel hooks for reload unit tests (avoid telegram/discord/lws linkage).
 */
#include "channels/channel.h"
#include "channels/heartbeat.h"
#include "core/config.h"

void shellclaw_telegram_set_live_cfg(const config_t *cfg)
{
	(void)cfg;
}

void shellclaw_discord_set_live_cfg(const config_t *cfg)
{
	(void)cfg;
}

void heartbeat_set_live_config(const config_t *cfg)
{
	(void)cfg;
}
