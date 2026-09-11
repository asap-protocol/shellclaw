/**
 * @file http_reload_stub.c
 * @brief No-op http_set_live_config for reload unit tests (avoid gateway/lws linkage).
 */
#include "gateway/http.h"
#include "core/config.h"

void http_set_live_config(const config_t *cfg)
{
	(void)cfg;
}
