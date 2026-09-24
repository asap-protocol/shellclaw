# ShellClaw Phase 1 — debug/release profiles, C11, libcurl for providers
CC     ?= gcc
BUILD  ?= debug
BINDIR ?= build
DSYMDIR ?= tests-dSYM
INC    := -I. -I tests -I src -I vendor/tomlc99 -I vendor/sqlite3 -I vendor/cJSON -Isrc/vendor/tweetnacl
LDLIBS := -lcurl -lm
# Gateway (Phase 2): libwebsockets for HTTP+WebSocket on same port
# Install: brew install libwebsockets
# Set GATEWAY=0 to build without gateway when libwebsockets not installed
GATEWAY ?= $(shell pkg-config --exists libwebsockets 2>/dev/null && echo 1 || echo 0)
GATEWAY_CFLAGS := $(if $(filter 1,$(GATEWAY)),$(shell pkg-config --cflags libwebsockets 2>/dev/null),)
GATEWAY_CFLAGS += $(if $(filter 1,$(GATEWAY)),$(shell pkg-config --cflags openssl 2>/dev/null),)
GATEWAY_LDLIBS := $(if $(filter 1,$(GATEWAY)),$(shell pkg-config --libs libwebsockets 2>/dev/null),)
GATEWAY_LDLIBS += $(if $(filter 1,$(GATEWAY)),$(shell pkg-config --libs openssl 2>/dev/null),)
# Hardware GPIO (Phase 5): libgpiod v2 API for Jetson / RPi (JetPack 6 / Bookworm)
# Set LIBGPIOD=0 when libgpiod-dev is not installed (stub backend only).
# pkg-config may find libgpiod 1.x on older distros; we require >= 2.0.
LIBGPIOD_PKG := $(shell pkg-config --exists libgpiod 2>/dev/null && pkg-config --atleast-version=2.0 libgpiod 2>/dev/null && echo 1 || echo 0)
LIBGPIOD ?= $(LIBGPIOD_PKG)
LIBGPIOD_CFLAGS := $(if $(filter 1,$(LIBGPIOD)),$(shell pkg-config --cflags libgpiod 2>/dev/null),)
LIBGPIOD_LDLIBS := $(if $(filter 1,$(LIBGPIOD)),$(shell pkg-config --libs libgpiod 2>/dev/null),)
LDFLAGS :=
# On macOS debug builds: generate .dSYM into tests-dSYM/ and remove any from BINDIR
DSYM_SCRIPT = @mkdir -p $(DSYMDIR) && ( [ "$$(uname)" != "Darwin" ] || [ "$(BUILD)" != "debug" ] || dsymutil $(BINDIR)/$@ -o $(DSYMDIR)/$@.dSYM 2>/dev/null ); rm -rf $(BINDIR)/$@.dSYM

ifeq ($(BUILD),release)
CFLAGS ?= -std=c11 -Wall -Wextra -Os -DNDEBUG -ffunction-sections -fdata-sections
# macOS ld uses -dead_strip; GNU ld uses --gc-sections
ifeq ($(shell uname 2>/dev/null),Darwin)
LDFLAGS += -Wl,-dead_strip
else
LDFLAGS += -Wl,--gc-sections
endif
else ifeq ($(BUILD),coverage)
CFLAGS ?= -std=c11 -Wall -Wextra -g -O0 -DDEBUG --coverage
LDFLAGS := --coverage
else
CFLAGS ?= -std=c11 -Wall -Wextra -g -O0 -DDEBUG
endif
CFLAGS += -Wformat=2 -Wformat-security
CFLAGS += $(if $(filter 1,$(GATEWAY)),-DSHELLCLAW_GATEWAY,)
CFLAGS += $(if $(filter 1,$(LIBGPIOD)),-DHAVE_LIBGPIOD,)
CFLAGS += $(LIBGPIOD_CFLAGS)
ifeq ($(CI),true)
CFLAGS += -Werror
endif
# Vendor code (toml, sqlite3, cJSON) may emit warnings with GCC on Linux; exclude -Werror
VENDOR_CFLAGS := $(filter-out -Werror,$(CFLAGS))
# TweetNaCl 20140427 (unmodified): sign-compare in FOR() and sigma[] string init on Clang/GCC -Wextra
# -Wunterminated-string-initialization exists on newer Clang/GCC only (not Ubuntu CI GCC 13).
TWEETNACL_WNO_UNTERM := $(shell $(CC) -Wno-error=unterminated-string-initialization -E -x c -o /dev/null /dev/null 2>&1 \
	| grep -qE 'unrecognized|unknown option|no option' || echo -Wno-error=unterminated-string-initialization)
TWEETNACL_CFLAGS := $(CFLAGS) -Wno-error=sign-compare $(TWEETNACL_WNO_UNTERM) -fwrapv
# cJSON (vendored): cJSON_CreateNumber casts the double to int unconditionally, which is UB for
# NaN/Inf (NaN fails both saturation comparisons and falls into the plain (int)cast). ShellClaw's
# JCS layer rejects NaN/Inf afterward (jcs_canonicalize returns -1), but the cJSON cast already
# tripped UBSan. -fno-sanitize=float-cast-overflow is a no-op without -fsanitize and only disables
# that one check for this vendored file; ShellClaw src/ keeps full UBSan coverage under test-sanitize.
CJSON_CFLAGS := $(VENDOR_CFLAGS) -fno-sanitize=float-cast-overflow

# Core
CONFIG_O  := src/core/config.o
MAIN_O    := src/core/main.o
MEMORY_O  := src/core/memory.o
SKILL_O   := src/core/skill.o
AGENT_O   := src/core/agent.o
DAEMON_O  := src/core/daemon.o
RELOAD_O  := src/core/reload.o
CONFIG_PATCH_O := src/core/config_patch.o
BOOTSTRAP_O := src/core/bootstrap.o
DISPATCH_O := src/core/dispatch.o
# Vendor
TOML_O   := vendor/tomlc99/toml.o
SQLITE3_O := vendor/sqlite3/sqlite3.o
# Providers (Task 4)
PROVIDER_COMMON_O := src/providers/provider_common.o
STUB_O            := src/providers/stub.o
CJSON_O     := vendor/cJSON/cJSON.o
TWEETNACL_O := src/vendor/tweetnacl/tweetnacl.o
ANTHROPIC_O := src/providers/anthropic.o
OPENAI_O   := src/providers/openai.o
OPENAI_COMPAT_O := src/providers/openai_compat.o
LOCAL_O    := src/providers/local.o
ROUTER_O   := src/providers/router.o
# Channels (Task 6)
CHANNEL_COMMON_O := src/channels/channel_common.o
CHANNEL_STUB_O   := src/channels/stub.o
CHANNEL_CLI_O    := src/channels/cli.o
CHANNEL_TG_O     := src/channels/telegram.o
CHANNEL_DISCORD_O := src/channels/discord.o
DISCORD_HELPERS_O := src/channels/discord_helpers.o
CHANNEL_HEARTBEAT_O := src/channels/heartbeat.o
# Gateway (Phase 2) - auth always built (no libwebsockets); http/ws when libwebsockets available
AUTH_O   := src/gateway/auth.o
CHANNEL_WEBCHAT_O := $(if $(filter 1,$(GATEWAY)),src/channels/webchat.o,)
STATIC_O := $(if $(filter 1,$(GATEWAY)),src/gateway/static.o,)
HTTP_O     := $(if $(filter 1,$(GATEWAY)),src/gateway/http.o,)
HTTP_LWS_O := $(if $(filter 1,$(GATEWAY)),src/gateway/http_lws.o,)
ROUTES_O   := $(if $(filter 1,$(GATEWAY)),src/gateway/routes.o,)
ROUTES_HARDWARE_O := $(if $(filter 1,$(GATEWAY)),src/gateway/routes_hardware.o,)
WS_O       := $(if $(filter 1,$(GATEWAY)),src/gateway/ws.o,)
# Tools (Task 7)
SHELL_O    := src/tools/shell.o
WEBSEARCH_O := src/tools/web_search.o
FILE_O     := src/tools/file.o
REGISTRY_O := src/tools/registry.o
CONTEXT_O  := src/tools/context.o
CONTEXT_CACHE_O := src/tools/context_cache.o
CONTEXT_HTTP_O := src/tools/context_http.o
CONTEXT_GEO_O := src/tools/context_geo.o
CRYPTO_O := src/crypto/crypto.o
JCS_O := src/crypto/jcs.o
CRYPTO_LINK := $(CRYPTO_O) $(TWEETNACL_O)
HARDWARE_STUB_O := src/hardware/hardware_stub.o
HARDWARE_INIT_O := src/hardware/hardware_init.o
HARDWARE_GPIO_SNAPSHOT_O := src/hardware/hardware_gpio_snapshot.o
HARDWARE_TEGRASTATS_O := src/hardware/hardware_tegrastats.o
HARDWARE_TOOLS_O := src/tools/hardware_tools.o src/tools/hardware_tools_helpers.o src/tools/hardware_tools_gpio.o src/tools/hardware_tools_i2c.o
BOARD_DETECT_O := src/hardware/board_detect.o
HARDWARE_LIBGPIOD_O := $(if $(filter 1,$(LIBGPIOD)),src/hardware/hardware_libgpiod.o,)
HARDWARE_I2C_O := src/hardware/hardware_i2c.o
HARDWARE_CAMERA_O := src/hardware/hardware_camera.o
CRON_O         := src/tools/cron.o
ASAP_INVOKE_O  := src/tools/asap_invoke.o
ASAP_INVOKE_TEST_O := $(BINDIR)/asap_invoke_test.o
# Sandbox (Phase 3 §5)
SANDBOX_CORE_O := src/sandbox/sandbox.o
SANDBOX_LANDLOCK_O := src/sandbox/sandbox_landlock.o
SANDBOX_O  := $(SANDBOX_CORE_O) $(SANDBOX_LANDLOCK_O)
ALLOWLIST_SCAN_O := src/sandbox/allowlist.o
ALLOWLIST_PATH_O := src/sandbox/allowlist_path.o
ALLOWLIST_O := $(ALLOWLIST_SCAN_O) $(ALLOWLIST_PATH_O)
MANIFEST_O := src/asap/manifest.o
MANIFEST_PROFILES_O := src/asap/manifest_profiles.o
MANIFEST_BUILD_O := src/asap/manifest_build.o
MANIFEST_SIGN_O := src/asap/manifest_sign.o
MANIFEST_KEYS_O := src/asap/manifest_keys.o
ENVELOPE_O := src/asap/envelope.o
ULID_O := src/asap/ulid.o
CLIENT_O := src/asap/client.o
ASAP_REGISTRY_O := src/asap/registry.o
SERVER_O := src/asap/server.o
ASAP_LOG_O := src/asap/log.o
RATE_LIMIT_O := $(if $(filter 1,$(GATEWAY)),src/gateway/rate_limit.o,)
ASAP_REGISTRY_TEST_O := $(BINDIR)/asap_registry_test.o
# Phase 3 ASAP unit binaries: keep in sync with `test`, `coverage`, and scripts/coverage.sh TESTS list.
ASAP_UNIT_TESTS := test_asap_envelope test_asap_ulid test_asap_client test_asap_registry test_asap_server test_asap_invoke test_asap_log
# Provider objects built with SHELLCLAW_TEST for negative/parse tests (CR-21)
ANTHROPIC_TEST_O := $(BINDIR)/anthropic_test.o
OPENAI_TEST_O    := $(BINDIR)/openai_test.o
LOCAL_TEST_O     := $(BINDIR)/local_test.o
CHANNEL_TG_TEST_O := $(BINDIR)/telegram_test.o
CONTEXT_TEST_O := $(BINDIR)/context_test.o
CONTEXT_HTTP_TEST_O := $(BINDIR)/context_http_test.o
CONTEXT_GEO_TEST_O := $(BINDIR)/context_geo_test.o
CONTEXT_CACHE_TEST_O := $(BINDIR)/context_cache_test.o
HEARTBEAT_TEST_O := $(BINDIR)/heartbeat_test.o
CONTEXT_TEST_OBJS := $(CONTEXT_TEST_O) $(CONTEXT_HTTP_TEST_O) $(CONTEXT_GEO_TEST_O) $(CONTEXT_CACHE_TEST_O)
BOOTSTRAP_DISPATCH_STUB_O := tests/stubs/bootstrap_dispatch_stub.o
TOOL_RELOAD_STUB_O := tests/stubs/tool_reload_stub.o
RELOAD_CHANNEL_STUB_O := tests/stubs/reload_channel_stub.o
HTTP_RELOAD_STUB_O := tests/stubs/http_reload_stub.o
CORE_OBJS := $(CONFIG_O) $(MAIN_O) $(MEMORY_O) $(SKILL_O) $(AGENT_O) $(DAEMON_O) $(RELOAD_O) $(CONFIG_PATCH_O) $(BOOTSTRAP_O) $(DISPATCH_O)
VENDOR_OBJS := $(TOML_O) $(SQLITE3_O) $(CJSON_O)
OBJS := $(CORE_OBJS) $(VENDOR_OBJS)
PROVIDER_OBJS := $(PROVIDER_COMMON_O) $(STUB_O) $(ROUTER_O) $(ANTHROPIC_O) $(OPENAI_COMPAT_O) $(OPENAI_O) $(LOCAL_O)
CHANNEL_OBJS := $(CHANNEL_COMMON_O) $(CHANNEL_CLI_O) $(CHANNEL_TG_O) $(CHANNEL_DISCORD_O) $(DISCORD_HELPERS_O) $(CHANNEL_HEARTBEAT_O) $(CHANNEL_WEBCHAT_O)
ASAP_HTTP_BODY_O := $(if $(filter 1,$(GATEWAY)),src/gateway/asap_http_body.o,)
GATEWAY_OBJS := $(AUTH_O) $(STATIC_O) $(HTTP_O) $(HTTP_LWS_O) $(ASAP_HTTP_BODY_O) $(ROUTES_O) $(ROUTES_HARDWARE_O) $(WS_O) $(RATE_LIMIT_O)
TOOL_OBJS := $(SHELL_O) $(WEBSEARCH_O) $(FILE_O) $(REGISTRY_O) $(CONTEXT_O) $(CONTEXT_CACHE_O) $(CONTEXT_HTTP_O) $(CONTEXT_GEO_O) $(CRYPTO_LINK) $(HARDWARE_STUB_O) $(HARDWARE_INIT_O) $(HARDWARE_GPIO_SNAPSHOT_O) $(HARDWARE_TEGRASTATS_O) $(HARDWARE_TOOLS_O) $(BOARD_DETECT_O) $(HARDWARE_I2C_O) $(HARDWARE_CAMERA_O) $(HARDWARE_LIBGPIOD_O) $(CRON_O) $(ASAP_INVOKE_O)
ASAP_OBJS := $(MANIFEST_O) $(MANIFEST_PROFILES_O) $(MANIFEST_BUILD_O) $(MANIFEST_SIGN_O) $(MANIFEST_KEYS_O) $(JCS_O) $(ENVELOPE_O) $(ULID_O) $(CLIENT_O) $(ASAP_REGISTRY_O) $(SERVER_O) $(ASAP_LOG_O)
SANDBOX_OBJS := $(SANDBOX_O) $(ALLOWLIST_O)
SHELLCLAW_OBJS := $(OBJS) $(PROVIDER_OBJS) $(CHANNEL_OBJS) $(GATEWAY_OBJS) $(ASAP_OBJS) $(TOOL_OBJS) $(SANDBOX_OBJS)
SQLITE_CFLAGS := -DSQLITE_ENABLE_FTS5

.PHONY: all debug release clean clean-root-dsym test test-sanitize test_tweetnacl_smoke shellclaw static coverage

all: debug

debug:
	$(MAKE) BUILD=debug shellclaw

release:
	$(MAKE) BUILD=release shellclaw

shellclaw: $(SHELLCLAW_OBJS)
	@mkdir -p $(BINDIR)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $(BINDIR)/$@ $(SHELLCLAW_OBJS) $(LDLIBS) $(GATEWAY_LDLIBS) $(LIBGPIOD_LDLIBS) -pthread
	$(DSYM_SCRIPT)
	@if [ "$(BUILD)" = "release" ]; then strip -s $(BINDIR)/$@ 2>/dev/null || true; fi

$(CONFIG_O): src/core/config.c src/core/config.h
	$(CC) $(CFLAGS) $(INC) -c -o $@ $<

$(MAIN_O): src/core/main.c src/asap/manifest.h src/core/agent.h src/core/config.h src/core/bootstrap.h src/core/daemon.h src/core/dispatch.h src/core/reload.h src/channels/channel.h src/hardware/board_detect.h src/providers/provider.h
	$(CC) $(CFLAGS) $(INC) -c -o $@ src/core/main.c

$(DAEMON_O): src/core/daemon.c src/core/daemon.h src/core/config.h
	$(CC) $(CFLAGS) $(INC) -c -o $@ src/core/daemon.c

$(RELOAD_O): src/core/reload.c src/core/reload.h src/core/bootstrap.h src/core/config.h src/channels/channel.h src/channels/heartbeat.h src/providers/provider.h src/tools/tool.h
	$(CC) $(CFLAGS) $(INC) -c -o $@ src/core/reload.c

$(CONFIG_PATCH_O): src/core/config_patch.c src/core/config_patch.h src/core/config.h vendor/cJSON/cJSON.h
	$(CC) $(CFLAGS) $(INC) -c -o $@ src/core/config_patch.c

$(BOOTSTRAP_O): src/core/bootstrap.c src/core/bootstrap.h src/asap/manifest.h src/core/agent.h src/core/config.h src/core/memory.h src/core/skill.h src/channels/channel.h src/channels/heartbeat.h src/providers/provider.h src/tools/tool.h src/tools/cron.h
	$(CC) $(CFLAGS) $(INC) -c -o $@ src/core/bootstrap.c

$(BOOTSTRAP_DISPATCH_STUB_O): tests/stubs/bootstrap_dispatch_stub.c src/core/bootstrap.h src/core/agent.h src/core/config.h src/providers/provider.h src/tools/tool.h
	$(CC) $(CFLAGS) $(INC) -c -o $@ tests/stubs/bootstrap_dispatch_stub.c

$(TOOL_RELOAD_STUB_O): tests/stubs/tool_reload_stub.c src/tools/tool.h
	$(CC) $(CFLAGS) $(INC) -c -o $@ tests/stubs/tool_reload_stub.c

$(RELOAD_CHANNEL_STUB_O): tests/stubs/reload_channel_stub.c src/channels/channel.h src/channels/heartbeat.h src/core/config.h
	$(CC) $(CFLAGS) $(INC) -c -o $@ tests/stubs/reload_channel_stub.c

$(HTTP_RELOAD_STUB_O): tests/stubs/http_reload_stub.c src/gateway/http.h src/core/config.h
	$(CC) $(CFLAGS) $(INC) -c -o $@ tests/stubs/http_reload_stub.c

$(DISPATCH_O): src/core/dispatch.c src/core/dispatch.h src/core/agent.h src/core/bootstrap.h src/core/memory.h src/channels/channel.h src/tools/cron.h
	$(CC) $(CFLAGS) $(INC) -c -o $@ src/core/dispatch.c

$(TOML_O): vendor/tomlc99/toml.c vendor/tomlc99/toml.h
	$(CC) $(VENDOR_CFLAGS) $(INC) -c -o $@ $<

$(SQLITE3_O): vendor/sqlite3/sqlite3.c vendor/sqlite3/sqlite3.h
	$(CC) $(VENDOR_CFLAGS) $(SQLITE_CFLAGS) $(INC) -c -o $@ $<

$(MEMORY_O): src/core/memory.c src/core/memory.h
	$(CC) $(CFLAGS) $(INC) -c -o $@ $<

$(SKILL_O): src/core/skill.c src/core/skill.h src/core/config.h
	$(CC) $(CFLAGS) $(INC) -c -o $@ src/core/skill.c

$(AGENT_O): src/core/agent.c src/core/agent.h src/core/config.h src/core/memory.h src/core/skill.h src/providers/provider.h
	$(CC) $(CFLAGS) $(INC) -c -o $@ src/core/agent.c

$(PROVIDER_COMMON_O): src/providers/provider_common.c src/providers/provider.h vendor/cJSON/cJSON.h
	$(CC) $(CFLAGS) $(INC) -c -o $@ src/providers/provider_common.c

$(STUB_O): src/providers/stub.c src/providers/provider.h
	$(CC) $(CFLAGS) $(INC) -c -o $@ src/providers/stub.c

$(CJSON_O): vendor/cJSON/cJSON.c vendor/cJSON/cJSON.h
	$(CC) $(CJSON_CFLAGS) $(INC) -c -o $@ vendor/cJSON/cJSON.c

$(TWEETNACL_O): src/vendor/tweetnacl/tweetnacl.c src/vendor/tweetnacl/tweetnacl.h
	$(CC) $(TWEETNACL_CFLAGS) $(INC) -c -o $@ src/vendor/tweetnacl/tweetnacl.c

$(ANTHROPIC_O): src/providers/anthropic.c src/providers/provider.h src/core/config.h
	$(CC) $(CFLAGS) $(INC) -c -o $@ src/providers/anthropic.c

$(ANTHROPIC_TEST_O): src/providers/anthropic.c src/providers/provider.h src/core/config.h
	@mkdir -p $(BINDIR)
	$(CC) $(CFLAGS) $(INC) -DSHELLCLAW_TEST -c -o $@ src/providers/anthropic.c

$(OPENAI_COMPAT_O): src/providers/openai_compat.c src/providers/openai_compat.h src/providers/provider.h vendor/cJSON/cJSON.h
	$(CC) $(CFLAGS) $(INC) -c -o $@ src/providers/openai_compat.c

$(OPENAI_O): src/providers/openai.c src/providers/openai_compat.h src/providers/provider.h src/core/config.h
	$(CC) $(CFLAGS) $(INC) -c -o $@ src/providers/openai.c

$(OPENAI_TEST_O): src/providers/openai.c src/providers/openai_compat.h src/providers/provider.h src/core/config.h
	@mkdir -p $(BINDIR)
	$(CC) $(CFLAGS) $(INC) -DSHELLCLAW_TEST -c -o $@ src/providers/openai.c

$(LOCAL_O): src/providers/local.c src/providers/openai_compat.h src/providers/provider.h src/core/config.h
	$(CC) $(CFLAGS) $(INC) -c -o $@ src/providers/local.c

$(LOCAL_TEST_O): src/providers/local.c src/providers/openai_compat.h src/providers/provider.h src/core/config.h
	@mkdir -p $(BINDIR)
	$(CC) $(CFLAGS) $(INC) -DSHELLCLAW_TEST -c -o $@ src/providers/local.c

$(ROUTER_O): src/providers/router.c src/providers/provider.h src/core/config.h vendor/cJSON/cJSON.h
	$(CC) $(CFLAGS) $(INC) -c -o $@ src/providers/router.c

$(CHANNEL_COMMON_O): src/channels/channel_common.c src/channels/channel.h
	$(CC) $(CFLAGS) $(INC) -c -o $@ src/channels/channel_common.c

$(CHANNEL_STUB_O): src/channels/stub.c src/channels/channel.h src/core/config.h
	$(CC) $(CFLAGS) $(INC) -c -o $@ src/channels/stub.c

$(CHANNEL_CLI_O): src/channels/cli.c src/channels/channel.h src/core/config.h
	$(CC) $(CFLAGS) $(INC) -c -o $@ src/channels/cli.c

$(CHANNEL_TG_O): src/channels/telegram.c src/channels/channel.h src/core/config.h
	$(CC) $(CFLAGS) $(INC) -c -o $@ src/channels/telegram.c

$(CHANNEL_DISCORD_O): src/channels/discord.c src/channels/channel.h src/channels/discord_helpers.h src/core/config.h
	$(CC) $(CFLAGS) $(INC) $(GATEWAY_CFLAGS) -c -o $@ src/channels/discord.c

$(DISCORD_HELPERS_O): src/channels/discord_helpers.c src/channels/discord_helpers.h src/channels/channel.h vendor/cJSON/cJSON.h
	$(CC) $(CFLAGS) $(INC) -c -o $@ src/channels/discord_helpers.c

$(CHANNEL_HEARTBEAT_O): src/channels/heartbeat.c src/channels/heartbeat.h src/channels/channel.h src/core/config.h
	$(CC) $(CFLAGS) $(INC) -c -o $@ src/channels/heartbeat.c

$(HEARTBEAT_TEST_O): src/channels/heartbeat.c src/channels/heartbeat.h src/channels/channel.h src/core/config.h
	@mkdir -p $(BINDIR)
	$(CC) $(CFLAGS) $(INC) -DSHELLCLAW_TEST -c -o $@ src/channels/heartbeat.c

$(CHANNEL_WEBCHAT_O): src/channels/webchat.c src/channels/channel.h src/channels/webchat.h src/core/config.h
	$(CC) $(CFLAGS) $(INC) $(GATEWAY_CFLAGS) -c -o $@ src/channels/webchat.c

$(AUTH_O): src/gateway/auth.c src/gateway/auth.h src/core/config.h src/crypto/crypto.h
	$(CC) $(CFLAGS) $(INC) -c -o $@ src/gateway/auth.c

WS_TEST_O := src/gateway/ws_test.o
$(WS_TEST_O): src/gateway/ws.c src/gateway/ws.h
	$(CC) $(CFLAGS) $(INC) -DSHELLCLAW_WS_TEST -pthread -c -o $@ src/gateway/ws.c

src/gateway/ui_assets.h: web/index.html web/hardware.html web/css/style.css web/js/dashboardView.js web/js/app.js web/js/hardwareView.js scripts/embed_ui.sh
	@mkdir -p src/gateway
	@chmod +x scripts/embed_ui.sh
	./scripts/embed_ui.sh

src/gateway/static.o: src/gateway/static.c src/gateway/static.h src/gateway/ui_assets.h
	$(CC) $(CFLAGS) $(INC) -c -o $@ src/gateway/static.c

$(JCS_O): src/crypto/jcs.c src/crypto/jcs.h vendor/cJSON/cJSON.h
	$(CC) $(CFLAGS) $(INC) -c -o $@ src/crypto/jcs.c

$(MANIFEST_O): src/asap/manifest.c src/asap/manifest.h
	$(CC) $(CFLAGS) $(INC) -c -o $@ src/asap/manifest.c

$(MANIFEST_PROFILES_O): src/asap/manifest_profiles.c src/asap/manifest_profiles.h src/core/config.h src/hardware/board_detect.h
	$(CC) $(CFLAGS) $(INC) -c -o $@ src/asap/manifest_profiles.c

$(MANIFEST_BUILD_O): src/asap/manifest_build.c src/asap/manifest_build.h src/asap/manifest_profiles.h src/core/config.h src/core/skill.h src/core/version.h src/hardware/board_detect.h vendor/cJSON/cJSON.h
	$(CC) $(CFLAGS) $(INC) -c -o $@ src/asap/manifest_build.c

$(MANIFEST_SIGN_O): src/asap/manifest_sign.c src/asap/manifest_sign.h src/asap/manifest_build.h src/asap/manifest_keys.h src/crypto/crypto.h src/crypto/jcs.h vendor/cJSON/cJSON.h
	$(CC) $(CFLAGS) $(INC) -c -o $@ src/asap/manifest_sign.c

$(MANIFEST_KEYS_O): src/asap/manifest_keys.c src/asap/manifest_keys.h src/core/config.h src/crypto/crypto.h
	$(CC) $(CFLAGS) $(INC) -c -o $@ src/asap/manifest_keys.c

$(ASAP_HTTP_BODY_O): src/gateway/asap_http_body.c src/gateway/asap_http_body.h src/gateway/http_lws.h
	$(CC) $(CFLAGS) $(INC) $(GATEWAY_CFLAGS) -c -o $@ src/gateway/asap_http_body.c

$(ENVELOPE_O): src/asap/envelope.c src/asap/envelope.h src/asap/asap_version.h vendor/cJSON/cJSON.h
	$(CC) $(CFLAGS) $(INC) -c -o $@ src/asap/envelope.c

$(ULID_O): src/asap/ulid.c src/asap/ulid.h src/crypto/crypto.h
	$(CC) $(CFLAGS) $(INC) -c -o $@ src/asap/ulid.c

$(CLIENT_O): src/asap/client.c src/asap/client.h src/asap/envelope.h src/asap/ulid.h src/core/config.h src/providers/provider.h
	$(CC) $(CFLAGS) $(INC) -c -o $@ src/asap/client.c

$(ASAP_REGISTRY_O): src/asap/registry.c src/asap/registry.h src/asap/client.h src/providers/provider.h vendor/cJSON/cJSON.h
	$(CC) $(CFLAGS) $(INC) -c -o $@ src/asap/registry.c

$(SERVER_O): src/asap/server.c src/asap/server.h src/asap/envelope.h src/asap/asap_version.h src/asap/ulid.h src/core/agent.h src/core/config.h src/core/memory.h src/providers/provider.h vendor/cJSON/cJSON.h
	$(CC) $(CFLAGS) $(INC) -pthread -c -o $@ src/asap/server.c

$(ASAP_LOG_O): src/asap/log.c src/asap/log.h
	$(CC) $(CFLAGS) $(INC) -pthread -c -o $@ src/asap/log.c

$(ASAP_REGISTRY_TEST_O): src/asap/registry.c src/asap/registry.h src/asap/client.h src/providers/provider.h vendor/cJSON/cJSON.h
	@mkdir -p $(BINDIR)
	$(CC) $(CFLAGS) $(INC) -DSHELLCLAW_REGISTRY_TEST -c -o $@ src/asap/registry.c

$(RATE_LIMIT_O): src/gateway/rate_limit.c src/gateway/rate_limit.h
	$(CC) $(CFLAGS) $(INC) -pthread -c -o $@ src/gateway/rate_limit.c

$(HTTP_O): src/gateway/http.c src/gateway/http.h src/gateway/http_lws.h src/gateway/ws.h src/providers/provider.h
	$(CC) $(CFLAGS) $(INC) $(GATEWAY_CFLAGS) -pthread -c -o $@ src/gateway/http.c

$(HTTP_LWS_O): src/gateway/http_lws.c src/gateway/http_lws.h src/gateway/asap_http_body.h src/gateway/routes.h src/gateway/auth.h src/gateway/static.h src/gateway/ws.h
	$(CC) $(CFLAGS) $(INC) $(GATEWAY_CFLAGS) -pthread -c -o $@ src/gateway/http_lws.c

$(ROUTES_O): src/gateway/routes.c src/gateway/routes.h src/gateway/routes_hardware.h src/gateway/http.h src/gateway/http_lws.h src/gateway/auth.h src/gateway/rate_limit.h src/tools/context.h src/asap/manifest.h src/asap/envelope.h src/asap/server.h src/asap/log.h src/core/bootstrap.h src/core/agent.h src/core/config.h src/core/config_patch.h src/core/reload.h src/core/memory.h src/core/skill.h src/providers/provider.h src/channels/channel.h src/tools/cron.h src/tools/tool.h
	$(CC) $(CFLAGS) $(INC) $(GATEWAY_CFLAGS) -pthread -c -o $@ src/gateway/routes.c

$(ROUTES_HARDWARE_O): src/gateway/routes_hardware.c src/gateway/routes_hardware.h src/gateway/routes.h src/gateway/http_lws.h src/gateway/uri_match.h src/hardware/hardware.h src/hardware/hardware_gpio_snapshot.h src/hardware/hardware_tegrastats.h src/hardware/board_detect.h src/core/config.h
	$(CC) $(CFLAGS) $(INC) $(GATEWAY_CFLAGS) -pthread -c -o $@ src/gateway/routes_hardware.c

$(WS_O): src/gateway/ws.c src/gateway/ws.h
	$(CC) $(CFLAGS) $(INC) $(GATEWAY_CFLAGS) -c -o $@ src/gateway/ws.c

$(SHELL_O): src/tools/shell.c src/tools/tool.h src/tools/shell.h src/core/config.h \
            src/sandbox/sandbox.h src/sandbox/allowlist.h
	$(CC) $(CFLAGS) $(INC) -c -o $@ src/tools/shell.c

$(SANDBOX_CORE_O): src/sandbox/sandbox.c src/sandbox/sandbox.h src/sandbox/sandbox_landlock.h
	$(CC) $(CFLAGS) $(INC) -c -o $@ src/sandbox/sandbox.c

$(SANDBOX_LANDLOCK_O): src/sandbox/sandbox_landlock.c src/sandbox/sandbox_landlock.h
	$(CC) $(CFLAGS) $(INC) -c -o $@ src/sandbox/sandbox_landlock.c

$(ALLOWLIST_SCAN_O): src/sandbox/allowlist.c src/sandbox/allowlist.h
	$(CC) $(CFLAGS) $(INC) -c -o $@ src/sandbox/allowlist.c

$(ALLOWLIST_PATH_O): src/sandbox/allowlist_path.c src/sandbox/allowlist.h
	$(CC) $(CFLAGS) $(INC) -c -o $@ src/sandbox/allowlist_path.c

$(WEBSEARCH_O): src/tools/web_search.c src/tools/tool.h src/tools/web_search.h src/core/config.h
	$(CC) $(CFLAGS) $(INC) -c -o $@ src/tools/web_search.c

$(FILE_O): src/tools/file.c src/tools/tool.h src/tools/file.h src/core/config.h
	$(CC) $(CFLAGS) $(INC) -c -o $@ src/tools/file.c

$(REGISTRY_O): src/tools/registry.c src/tools/tool.h src/tools/shell.h src/tools/web_search.h src/tools/file.h src/tools/context.h src/tools/asap_invoke.h src/tools/hardware_tools.h src/hardware/hardware.h src/core/config.h
	$(CC) $(CFLAGS) $(INC) -c -o $@ src/tools/registry.c

$(HARDWARE_INIT_O): src/hardware/hardware_init.c src/hardware/hardware.h src/hardware/board_detect.h src/hardware/boards/jetson_orin_nano.h src/hardware/boards/rpi_zero2w.h src/core/config.h
	$(CC) $(CFLAGS) $(INC) -c -o $@ src/hardware/hardware_init.c

$(HARDWARE_GPIO_SNAPSHOT_O): src/hardware/hardware_gpio_snapshot.c src/hardware/hardware_gpio_snapshot.h src/hardware/hardware.h src/hardware/board_detect.h
	$(CC) $(CFLAGS) $(INC) -c -o $@ src/hardware/hardware_gpio_snapshot.c

$(HARDWARE_TEGRASTATS_O): src/hardware/hardware_tegrastats.c src/hardware/hardware_tegrastats.h vendor/cJSON/cJSON.h
	$(CC) $(CFLAGS) $(INC) -c -o $@ src/hardware/hardware_tegrastats.c

$(HARDWARE_TOOLS_O): src/tools/hardware_tools.c src/tools/hardware_tools_helpers.c src/tools/hardware_tools_gpio.c src/tools/hardware_tools_i2c.c src/tools/hardware_tools.h src/tools/hardware_tools_internal.h src/tools/tool.h src/core/config.h src/hardware/hardware.h src/hardware/board_detect.h vendor/cJSON/cJSON.h
	$(CC) $(CFLAGS) $(INC) -c -o src/tools/hardware_tools.o src/tools/hardware_tools.c
	$(CC) $(CFLAGS) $(INC) -c -o src/tools/hardware_tools_helpers.o src/tools/hardware_tools_helpers.c
	$(CC) $(CFLAGS) $(INC) -c -o src/tools/hardware_tools_gpio.o src/tools/hardware_tools_gpio.c
	$(CC) $(CFLAGS) $(INC) -c -o src/tools/hardware_tools_i2c.o src/tools/hardware_tools_i2c.c

$(CRYPTO_O): src/crypto/crypto.c src/crypto/crypto.h
	$(CC) $(CFLAGS) $(INC) -c -o $@ src/crypto/crypto.c

$(CONTEXT_O): src/tools/context.c src/tools/context.h src/tools/context_internal.h src/tools/tool.h src/core/config.h vendor/cJSON/cJSON.h
	$(CC) $(CFLAGS) $(INC) -pthread -c -o $@ src/tools/context.c

$(CONTEXT_CACHE_O): src/tools/context_cache.c src/tools/context_internal.h src/core/config.h vendor/cJSON/cJSON.h
	$(CC) $(CFLAGS) $(INC) -pthread -c -o $@ src/tools/context_cache.c

$(CONTEXT_HTTP_O): src/tools/context_http.c src/tools/context_internal.h
	$(CC) $(CFLAGS) $(INC) -pthread -c -o $@ src/tools/context_http.c

$(CONTEXT_GEO_O): src/tools/context_geo.c src/tools/context_internal.h src/core/config.h vendor/cJSON/cJSON.h
	$(CC) $(CFLAGS) $(INC) -pthread -c -o $@ src/tools/context_geo.c

$(CONTEXT_TEST_O): src/tools/context.c src/tools/context.h src/tools/context_internal.h src/tools/tool.h src/core/config.h vendor/cJSON/cJSON.h
	@mkdir -p $(BINDIR)
	$(CC) $(CFLAGS) $(INC) -DSHELLCLAW_CONTEXT_TEST -pthread -c -o $@ src/tools/context.c

$(CONTEXT_HTTP_TEST_O): src/tools/context_http.c src/tools/context_internal.h
	@mkdir -p $(BINDIR)
	$(CC) $(CFLAGS) $(INC) -DSHELLCLAW_CONTEXT_TEST -pthread -c -o $@ src/tools/context_http.c

$(CONTEXT_GEO_TEST_O): src/tools/context_geo.c src/tools/context_internal.h src/core/config.h vendor/cJSON/cJSON.h
	@mkdir -p $(BINDIR)
	$(CC) $(CFLAGS) $(INC) -DSHELLCLAW_CONTEXT_TEST -pthread -c -o $@ src/tools/context_geo.c

$(CONTEXT_CACHE_TEST_O): src/tools/context_cache.c src/tools/context_internal.h src/core/config.h vendor/cJSON/cJSON.h
	@mkdir -p $(BINDIR)
	$(CC) $(CFLAGS) $(INC) -DSHELLCLAW_CONTEXT_TEST -pthread -c -o $@ src/tools/context_cache.c

$(HARDWARE_STUB_O): src/hardware/hardware_stub.c src/hardware/hardware.h src/core/config.h
	$(CC) $(CFLAGS) $(INC) -c -o $@ src/hardware/hardware_stub.c

$(BOARD_DETECT_O): src/hardware/board_detect.c src/hardware/board_detect.h
	$(CC) $(CFLAGS) $(INC) -c -o $@ src/hardware/board_detect.c

$(HARDWARE_LIBGPIOD_O): src/hardware/hardware_libgpiod.c src/hardware/hardware_libgpiod.h src/hardware/pin_table.h
	$(CC) $(CFLAGS) $(INC) -c -o $@ src/hardware/hardware_libgpiod.c

$(HARDWARE_I2C_O): src/hardware/hardware_i2c.c src/hardware/hardware_i2c.h
	$(CC) $(CFLAGS) $(INC) -c -o $@ src/hardware/hardware_i2c.c

$(HARDWARE_CAMERA_O): src/hardware/hardware_camera.c src/hardware/hardware_camera.h src/hardware/board_detect.h
	$(CC) $(CFLAGS) $(INC) -c -o $@ src/hardware/hardware_camera.c

$(CRON_O): src/tools/cron.c src/tools/cron.h src/crypto/crypto.h src/core/memory.h src/channels/channel.h src/core/config.h
	$(CC) $(CFLAGS) $(INC) -c -o $@ src/tools/cron.c

$(ASAP_INVOKE_O): src/tools/asap_invoke.c src/tools/asap_invoke.h src/tools/tool.h src/asap/envelope.h src/asap/client.h src/asap/registry.h src/asap/ulid.h src/asap/asap_version.h src/core/config.h vendor/cJSON/cJSON.h
	$(CC) $(CFLAGS) $(INC) -c -o $@ src/tools/asap_invoke.c

$(ASAP_INVOKE_TEST_O): src/tools/asap_invoke.c src/tools/asap_invoke.h src/tools/tool.h src/asap/envelope.h src/asap/client.h src/asap/registry.h src/asap/ulid.h src/asap/asap_version.h src/core/config.h vendor/cJSON/cJSON.h
	@mkdir -p $(BINDIR)
	$(CC) $(CFLAGS) $(INC) -DSHELLCLAW_ASAP_INVOKE_TEST -DSHELLCLAW_REGISTRY_TEST -c -o $@ src/tools/asap_invoke.c

test_config: tests/test_config.c $(CONFIG_O) $(TOML_O)
	@mkdir -p $(BINDIR)
	$(CC) $(CFLAGS) $(LDFLAGS) $(INC) -o $(BINDIR)/$@ tests/test_config.c $(CONFIG_O) $(TOML_O) $(LDLIBS)
	$(DSYM_SCRIPT)

test_config_patch: tests/test_config_patch.c $(CONFIG_PATCH_O) $(CONFIG_O) $(TOML_O) $(CJSON_O)
	@mkdir -p $(BINDIR)
	$(CC) $(CFLAGS) $(LDFLAGS) $(INC) -o $(BINDIR)/$@ tests/test_config_patch.c $(CONFIG_PATCH_O) $(CONFIG_O) $(TOML_O) $(CJSON_O) $(LDLIBS)
	$(DSYM_SCRIPT)

test_memory: tests/test_memory.c $(MEMORY_O) $(SQLITE3_O)
	@mkdir -p $(BINDIR)
	$(CC) $(CFLAGS) $(LDFLAGS) $(INC) -o $(BINDIR)/$@ tests/test_memory.c $(MEMORY_O) $(SQLITE3_O) $(LDLIBS)
	$(DSYM_SCRIPT)

test_skill: tests/test_skill.c $(SKILL_O) $(CONFIG_O) $(TOML_O)
	@mkdir -p $(BINDIR)
	$(CC) $(CFLAGS) $(LDFLAGS) $(INC) -o $(BINDIR)/$@ tests/test_skill.c $(SKILL_O) $(CONFIG_O) $(TOML_O) $(LDLIBS)
	$(DSYM_SCRIPT)

test_provider: tests/test_provider.c $(STUB_O) $(PROVIDER_COMMON_O) $(CJSON_O)
	@mkdir -p $(BINDIR)
	$(CC) $(CFLAGS) $(LDFLAGS) $(INC) -o $(BINDIR)/$@ tests/test_provider.c $(STUB_O) $(PROVIDER_COMMON_O) $(CJSON_O) $(LDLIBS)
	$(DSYM_SCRIPT)

test_anthropic: tests/test_anthropic.c $(ANTHROPIC_TEST_O) $(PROVIDER_COMMON_O) $(CONFIG_O) $(TOML_O) $(CJSON_O)
	@mkdir -p $(BINDIR)
	$(CC) $(CFLAGS) $(LDFLAGS) $(INC) -DSHELLCLAW_TEST -o $(BINDIR)/$@ tests/test_anthropic.c $(ANTHROPIC_TEST_O) $(PROVIDER_COMMON_O) $(CONFIG_O) $(TOML_O) $(CJSON_O) $(LDLIBS)
	$(DSYM_SCRIPT)

test_openai: tests/test_openai.c $(OPENAI_TEST_O) $(OPENAI_COMPAT_O) $(PROVIDER_COMMON_O) $(CONFIG_O) $(TOML_O) $(CJSON_O)
	@mkdir -p $(BINDIR)
	$(CC) $(CFLAGS) $(LDFLAGS) $(INC) -DSHELLCLAW_TEST -o $(BINDIR)/$@ tests/test_openai.c $(OPENAI_TEST_O) $(OPENAI_COMPAT_O) $(PROVIDER_COMMON_O) $(CONFIG_O) $(TOML_O) $(CJSON_O) $(LDLIBS)
	$(DSYM_SCRIPT)

test_local_provider: tests/test_local_provider.c $(LOCAL_TEST_O) $(OPENAI_COMPAT_O) $(PROVIDER_COMMON_O) $(CONFIG_O) $(TOML_O) $(CJSON_O)
	@mkdir -p $(BINDIR)
	$(CC) $(CFLAGS) $(LDFLAGS) $(INC) -DSHELLCLAW_TEST -o $(BINDIR)/$@ tests/test_local_provider.c $(LOCAL_TEST_O) $(OPENAI_COMPAT_O) $(PROVIDER_COMMON_O) $(CONFIG_O) $(TOML_O) $(CJSON_O) $(LDLIBS)
	$(DSYM_SCRIPT)

test_router: tests/test_router.c $(ROUTER_O) $(STUB_O) $(ANTHROPIC_O) $(OPENAI_COMPAT_O) $(OPENAI_O) $(LOCAL_O) $(PROVIDER_COMMON_O) $(CONFIG_O) $(TOML_O) $(CJSON_O)
	@mkdir -p $(BINDIR)
	$(CC) $(CFLAGS) $(LDFLAGS) $(INC) -o $(BINDIR)/$@ tests/test_router.c $(ROUTER_O) $(STUB_O) $(ANTHROPIC_O) $(OPENAI_COMPAT_O) $(OPENAI_O) $(LOCAL_O) $(PROVIDER_COMMON_O) $(CONFIG_O) $(TOML_O) $(CJSON_O) $(LDLIBS) -pthread
	$(DSYM_SCRIPT)

test_heartbeat: tests/test_heartbeat.c $(HEARTBEAT_TEST_O) $(CHANNEL_COMMON_O) $(CONFIG_O) $(TOML_O)
	@mkdir -p $(BINDIR)
	$(CC) $(CFLAGS) $(LDFLAGS) $(INC) -DSHELLCLAW_TEST -o $(BINDIR)/$@ tests/test_heartbeat.c $(HEARTBEAT_TEST_O) $(CHANNEL_COMMON_O) $(CONFIG_O) $(TOML_O) $(LDLIBS)
	$(DSYM_SCRIPT)

test_crypto: tests/test_crypto.c $(CRYPTO_LINK)
	@mkdir -p $(BINDIR)
	$(CC) $(CFLAGS) $(LDFLAGS) $(INC) -o $(BINDIR)/$@ tests/test_crypto.c $(CRYPTO_LINK) $(LDLIBS)
	$(DSYM_SCRIPT)

test_tweetnacl_smoke: tests/test_tweetnacl_smoke.c $(TWEETNACL_O)
	@mkdir -p $(BINDIR)
	$(CC) $(CFLAGS) $(LDFLAGS) $(INC) -o $(BINDIR)/$@ tests/test_tweetnacl_smoke.c $(TWEETNACL_O) $(LDLIBS)
	$(BINDIR)/$@
	$(DSYM_SCRIPT)

test_hardware_stub: tests/test_hardware_stub.c $(HARDWARE_STUB_O) $(CONFIG_O) $(TOML_O)
	@mkdir -p $(BINDIR)
	$(CC) $(CFLAGS) $(LDFLAGS) $(INC) -o $(BINDIR)/$@ tests/test_hardware_stub.c $(HARDWARE_STUB_O) $(CONFIG_O) $(TOML_O) $(LDLIBS)
	$(DSYM_SCRIPT)

test_board_detect: tests/test_board_detect.c $(BOARD_DETECT_O)
	@mkdir -p $(BINDIR)
	$(CC) $(CFLAGS) $(LDFLAGS) $(INC) -o $(BINDIR)/$@ tests/test_board_detect.c $(BOARD_DETECT_O) $(LDLIBS)
	$(DSYM_SCRIPT)

test_hardware_libgpiod: tests/test_hardware_libgpiod.c $(HARDWARE_LIBGPIOD_O)
	@mkdir -p $(BINDIR)
	$(CC) $(CFLAGS) $(LDFLAGS) $(INC) -DSHELLCLAW_HARDWARE_LIBGPIOD_TEST -o $(BINDIR)/$@ tests/test_hardware_libgpiod.c $(HARDWARE_LIBGPIOD_O) $(LDLIBS) $(LIBGPIOD_LDLIBS) -pthread
	$(DSYM_SCRIPT)

test_hardware_i2c: tests/test_hardware_i2c.c $(HARDWARE_I2C_O)
	@mkdir -p $(BINDIR)
	$(CC) $(CFLAGS) $(LDFLAGS) $(INC) -o $(BINDIR)/$@ tests/test_hardware_i2c.c $(HARDWARE_I2C_O) $(LDLIBS)

test_hardware_camera: tests/test_hardware_camera.c $(HARDWARE_CAMERA_O)
	@mkdir -p $(BINDIR)
	$(CC) $(CFLAGS) $(LDFLAGS) $(INC) -o $(BINDIR)/$@ tests/test_hardware_camera.c $(HARDWARE_CAMERA_O) $(LDLIBS)

test_pin_tables: tests/test_pin_tables.c
	@mkdir -p $(BINDIR)
	$(CC) $(CFLAGS) $(LDFLAGS) $(INC) -o $(BINDIR)/$@ tests/test_pin_tables.c $(LDLIBS)

test_hardware_init: tests/test_hardware_init.c $(HARDWARE_INIT_O) $(HARDWARE_STUB_O) $(BOARD_DETECT_O) $(HARDWARE_I2C_O) $(HARDWARE_CAMERA_O) $(HARDWARE_LIBGPIOD_O) $(CONFIG_O) $(TOML_O)
	@mkdir -p $(BINDIR)
	$(CC) $(CFLAGS) $(LDFLAGS) $(INC) -o $(BINDIR)/$@ tests/test_hardware_init.c $(HARDWARE_INIT_O) $(HARDWARE_STUB_O) $(BOARD_DETECT_O) $(HARDWARE_I2C_O) $(HARDWARE_CAMERA_O) $(HARDWARE_LIBGPIOD_O) $(CONFIG_O) $(TOML_O) $(LDLIBS) $(LIBGPIOD_LDLIBS) -pthread

test_hardware_gpio_snapshot: tests/test_hardware_gpio_snapshot.c $(HARDWARE_GPIO_SNAPSHOT_O) $(HARDWARE_INIT_O) $(HARDWARE_STUB_O) $(BOARD_DETECT_O) $(HARDWARE_I2C_O) $(HARDWARE_CAMERA_O) $(HARDWARE_LIBGPIOD_O) $(CONFIG_O) $(TOML_O) $(CJSON_O)
	@mkdir -p $(BINDIR)
	$(CC) $(CFLAGS) $(LDFLAGS) $(INC) -o $(BINDIR)/$@ tests/test_hardware_gpio_snapshot.c $(HARDWARE_GPIO_SNAPSHOT_O) $(HARDWARE_INIT_O) $(HARDWARE_STUB_O) $(BOARD_DETECT_O) $(HARDWARE_I2C_O) $(HARDWARE_CAMERA_O) $(HARDWARE_LIBGPIOD_O) $(CONFIG_O) $(TOML_O) $(CJSON_O) $(LDLIBS) $(LIBGPIOD_LDLIBS) -pthread

test_hardware_tegrastats: tests/test_hardware_tegrastats.c $(HARDWARE_TEGRASTATS_O) $(CJSON_O)
	@mkdir -p $(BINDIR)
	$(CC) $(CFLAGS) $(LDFLAGS) $(INC) -o $(BINDIR)/$@ tests/test_hardware_tegrastats.c $(HARDWARE_TEGRASTATS_O) $(CJSON_O) $(LDLIBS)

test_registry: tests/test_registry.c $(REGISTRY_O) $(HARDWARE_TOOLS_O) $(HARDWARE_INIT_O) $(HARDWARE_STUB_O) $(BOARD_DETECT_O) $(HARDWARE_I2C_O) $(HARDWARE_CAMERA_O) $(HARDWARE_LIBGPIOD_O) $(CONFIG_O) $(TOML_O) $(CJSON_O)
	@mkdir -p $(BINDIR)
	$(CC) $(CFLAGS) $(LDFLAGS) $(INC) -o $(BINDIR)/$@ tests/test_registry.c $(REGISTRY_O) $(HARDWARE_TOOLS_O) $(HARDWARE_INIT_O) $(HARDWARE_STUB_O) $(BOARD_DETECT_O) $(HARDWARE_I2C_O) $(HARDWARE_CAMERA_O) $(HARDWARE_LIBGPIOD_O) $(CONFIG_O) $(TOML_O) $(CJSON_O) $(LDLIBS) $(LIBGPIOD_LDLIBS) -pthread
	$(DSYM_SCRIPT)

test_hardware_tools: tests/test_hardware_tools.c $(HARDWARE_TOOLS_O) $(HARDWARE_INIT_O) $(HARDWARE_STUB_O) $(BOARD_DETECT_O) $(HARDWARE_I2C_O) $(HARDWARE_CAMERA_O) $(HARDWARE_LIBGPIOD_O) $(CONFIG_O) $(TOML_O) $(CJSON_O)
	@mkdir -p $(BINDIR)
	$(CC) $(CFLAGS) $(LDFLAGS) $(INC) -o $(BINDIR)/$@ tests/test_hardware_tools.c $(HARDWARE_TOOLS_O) $(HARDWARE_INIT_O) $(HARDWARE_STUB_O) $(BOARD_DETECT_O) $(HARDWARE_I2C_O) $(HARDWARE_CAMERA_O) $(HARDWARE_LIBGPIOD_O) $(CONFIG_O) $(TOML_O) $(CJSON_O) $(LDLIBS) $(LIBGPIOD_LDLIBS) -pthread
	$(DSYM_SCRIPT)

test_agent: tests/test_agent.c $(AGENT_O) $(ROUTER_O) $(STUB_O) $(ANTHROPIC_O) $(OPENAI_COMPAT_O) $(OPENAI_O) $(LOCAL_O) $(PROVIDER_COMMON_O) $(CONFIG_O) $(TOML_O) $(MEMORY_O) $(SKILL_O) $(SQLITE3_O) $(CJSON_O)
	@mkdir -p $(BINDIR)
	$(CC) $(CFLAGS) $(LDFLAGS) $(INC) -o $(BINDIR)/$@ tests/test_agent.c $(AGENT_O) $(ROUTER_O) $(STUB_O) $(ANTHROPIC_O) $(OPENAI_COMPAT_O) $(OPENAI_O) $(LOCAL_O) $(PROVIDER_COMMON_O) $(CONFIG_O) $(TOML_O) $(MEMORY_O) $(SKILL_O) $(SQLITE3_O) $(CJSON_O) $(LDLIBS) -pthread
	$(DSYM_SCRIPT)

test_channel: tests/test_channel.c $(CHANNEL_COMMON_O) $(CHANNEL_STUB_O)
	@mkdir -p $(BINDIR)
	$(CC) $(CFLAGS) $(LDFLAGS) $(INC) -o $(BINDIR)/$@ tests/test_channel.c $(CHANNEL_COMMON_O) $(CHANNEL_STUB_O) $(LDLIBS)
	$(DSYM_SCRIPT)

test_cli: tests/test_cli.c $(CHANNEL_COMMON_O) $(CHANNEL_CLI_O)
	@mkdir -p $(BINDIR)
	$(CC) $(CFLAGS) $(LDFLAGS) $(INC) -o $(BINDIR)/$@ tests/test_cli.c $(CHANNEL_COMMON_O) $(CHANNEL_CLI_O) $(LDLIBS)
	$(DSYM_SCRIPT)

test_shell: tests/test_shell.c $(SHELL_O) $(SANDBOX_O) $(ALLOWLIST_O) $(CONFIG_O) $(TOML_O) $(CJSON_O)
	@mkdir -p $(BINDIR)
	$(CC) $(CFLAGS) $(LDFLAGS) $(INC) -o $(BINDIR)/$@ tests/test_shell.c $(SHELL_O) $(SANDBOX_O) $(ALLOWLIST_O) $(CONFIG_O) $(TOML_O) $(CJSON_O) $(LDLIBS)
	$(DSYM_SCRIPT)

test_file: tests/test_file.c $(FILE_O) $(REGISTRY_O) $(ALLOWLIST_O) $(CONFIG_O) $(TOML_O) $(CJSON_O)
	@mkdir -p $(BINDIR)
	$(CC) $(CFLAGS) $(LDFLAGS) $(INC) -o $(BINDIR)/$@ tests/test_file.c $(FILE_O) $(ALLOWLIST_O) $(CONFIG_O) $(TOML_O) $(CJSON_O) $(LDLIBS)
	$(DSYM_SCRIPT)

$(CHANNEL_TG_TEST_O): src/channels/telegram.c src/channels/channel.h src/core/config.h
	@mkdir -p $(BINDIR)
	$(CC) $(CFLAGS) $(INC) -DSHELLCLAW_TEST -c -o $@ src/channels/telegram.c

test_telegram: tests/test_telegram.c $(CHANNEL_TG_TEST_O) $(CHANNEL_COMMON_O) $(CONFIG_O) $(TOML_O) $(CJSON_O)
	@mkdir -p $(BINDIR)
	$(CC) $(CFLAGS) $(LDFLAGS) $(INC) -DSHELLCLAW_TEST -o $(BINDIR)/$@ tests/test_telegram.c $(CHANNEL_TG_TEST_O) $(CHANNEL_COMMON_O) $(CONFIG_O) $(TOML_O) $(CJSON_O) $(LDLIBS)
	$(DSYM_SCRIPT)

test_discord_helpers: tests/test_discord_helpers.c $(DISCORD_HELPERS_O) $(CJSON_O)
	@mkdir -p $(BINDIR)
	$(CC) $(CFLAGS) $(LDFLAGS) $(INC) -o $(BINDIR)/$@ tests/test_discord_helpers.c $(DISCORD_HELPERS_O) $(CJSON_O) $(LDLIBS)
	$(DSYM_SCRIPT)

test_web_search: tests/test_web_search.c $(WEBSEARCH_O) $(CONFIG_O) $(TOML_O) $(CJSON_O)
	@mkdir -p $(BINDIR)
	$(CC) $(CFLAGS) $(LDFLAGS) $(INC) -o $(BINDIR)/$@ tests/test_web_search.c $(WEBSEARCH_O) $(CONFIG_O) $(TOML_O) $(CJSON_O) $(LDLIBS)
	$(DSYM_SCRIPT)

test_cron: tests/test_cron.c $(CRON_O) $(CRYPTO_LINK) $(MEMORY_O) $(SQLITE3_O) $(CHANNEL_COMMON_O) $(CJSON_O)
	@mkdir -p $(BINDIR)
	$(CC) $(CFLAGS) $(LDFLAGS) $(INC) -o $(BINDIR)/$@ tests/test_cron.c $(CRON_O) $(CRYPTO_LINK) $(MEMORY_O) $(SQLITE3_O) $(CHANNEL_COMMON_O) $(CJSON_O) $(LDLIBS)

test_ws: tests/test_ws.c $(WS_TEST_O)
	@mkdir -p $(BINDIR)
	$(CC) $(CFLAGS) $(LDFLAGS) $(INC) -o $(BINDIR)/$@ tests/test_ws.c $(WS_TEST_O) $(LDLIBS) -pthread
	$(DSYM_SCRIPT)

test_context: tests/test_context.c $(CONTEXT_TEST_OBJS) $(CONFIG_O) $(TOML_O) $(CJSON_O)
	@mkdir -p $(BINDIR)
	$(CC) $(CFLAGS) $(LDFLAGS) $(INC) -DSHELLCLAW_CONTEXT_TEST -o $(BINDIR)/$@ tests/test_context.c $(CONTEXT_TEST_OBJS) $(CONFIG_O) $(TOML_O) $(CJSON_O) $(LDLIBS) -pthread
	$(DSYM_SCRIPT)

test_dispatch: tests/test_dispatch.c $(DISPATCH_O) $(BOOTSTRAP_DISPATCH_STUB_O) $(AGENT_O) $(ROUTER_O) $(STUB_O) $(ANTHROPIC_O) $(OPENAI_COMPAT_O) $(OPENAI_O) $(LOCAL_O) $(PROVIDER_COMMON_O) $(MEMORY_O) $(SQLITE3_O) $(SKILL_O) $(CONFIG_O) $(TOML_O) $(CJSON_O) $(CRON_O) $(CRYPTO_LINK) $(CHANNEL_COMMON_O)
	@mkdir -p $(BINDIR)
	$(CC) $(CFLAGS) $(LDFLAGS) $(INC) -o $(BINDIR)/$@ tests/test_dispatch.c $(DISPATCH_O) $(BOOTSTRAP_DISPATCH_STUB_O) $(AGENT_O) $(ROUTER_O) $(STUB_O) $(ANTHROPIC_O) $(OPENAI_COMPAT_O) $(OPENAI_O) $(LOCAL_O) $(PROVIDER_COMMON_O) $(MEMORY_O) $(SQLITE3_O) $(SKILL_O) $(CONFIG_O) $(TOML_O) $(CJSON_O) $(CRON_O) $(CRYPTO_LINK) $(CHANNEL_COMMON_O) $(LDLIBS) -pthread
	$(DSYM_SCRIPT)

RELOAD_TEST_OBJS := $(RELOAD_O) $(BOOTSTRAP_DISPATCH_STUB_O) $(TOOL_RELOAD_STUB_O) $(RELOAD_CHANNEL_STUB_O) $(HTTP_RELOAD_STUB_O) $(CONFIG_O) $(TOML_O) \
	$(ROUTER_O) $(STUB_O) $(ANTHROPIC_O) $(OPENAI_COMPAT_O) $(OPENAI_O) $(LOCAL_O) $(PROVIDER_COMMON_O) $(CJSON_O)


.PHONY: test_reload_exe
test_reload:
	@$(MAKE) GATEWAY=0 test_reload_exe

test_reload_exe: tests/test_reload.c $(RELOAD_TEST_OBJS)
	@mkdir -p $(BINDIR)
	$(CC) $(CFLAGS) $(LDFLAGS) $(INC) -o $(BINDIR)/test_reload tests/test_reload.c $(RELOAD_TEST_OBJS) $(LDLIBS) -pthread
	$(DSYM_SCRIPT)


test_auth: tests/test_auth.c $(AUTH_O) $(CRYPTO_LINK) $(CJSON_O) $(CONFIG_O) $(TOML_O)
	@mkdir -p $(BINDIR)
	$(CC) $(CFLAGS) $(LDFLAGS) $(INC) -o $(BINDIR)/$@ tests/test_auth.c $(AUTH_O) $(CRYPTO_LINK) $(CJSON_O) $(CONFIG_O) $(TOML_O) $(LDLIBS)
	$(DSYM_SCRIPT)

MANIFEST_TEST_OBJS := $(MANIFEST_O) $(MANIFEST_PROFILES_O) $(MANIFEST_BUILD_O) $(MANIFEST_SIGN_O) $(MANIFEST_KEYS_O) $(CONFIG_O) $(TOML_O) $(SKILL_O) $(BOARD_DETECT_O) $(CRYPTO_O) $(JCS_O) $(TWEETNACL_O) $(CJSON_O)

test_manifest_build: tests/test_manifest_build.c tests/manifest_test_common.h $(MANIFEST_TEST_OBJS)
	@mkdir -p $(BINDIR)
	$(CC) $(CFLAGS) $(LDFLAGS) $(INC) -o $(BINDIR)/$@ tests/test_manifest_build.c $(MANIFEST_TEST_OBJS) $(LDLIBS)
	$(DSYM_SCRIPT)

test_manifest_keys: tests/test_manifest_keys.c $(MANIFEST_KEYS_O) $(CRYPTO_O) $(TWEETNACL_O) $(CONFIG_O) $(TOML_O)
	@mkdir -p $(BINDIR)
	$(CC) $(CFLAGS) $(LDFLAGS) $(INC) -o $(BINDIR)/$@ tests/test_manifest_keys.c $(MANIFEST_KEYS_O) $(CRYPTO_O) $(TWEETNACL_O) $(CONFIG_O) $(TOML_O) $(LDLIBS)
	$(DSYM_SCRIPT)

test_jcs: tests/test_jcs.c $(JCS_O) $(CJSON_O)
	@mkdir -p $(BINDIR)
	$(CC) $(CFLAGS) $(LDFLAGS) $(INC) -o $(BINDIR)/$@ tests/test_jcs.c $(JCS_O) $(CJSON_O) $(LDLIBS)
	$(DSYM_SCRIPT)

test_manifest: test_manifest_build test_manifest_keys test_jcs
	$(BINDIR)/test_manifest_build
	$(BINDIR)/test_manifest_keys
	$(BINDIR)/test_jcs

test_asap_envelope: tests/test_asap_envelope.c $(ENVELOPE_O) $(CJSON_O)
	@mkdir -p $(BINDIR)
	$(CC) $(CFLAGS) $(LDFLAGS) $(INC) -o $(BINDIR)/$@ tests/test_asap_envelope.c $(ENVELOPE_O) $(CJSON_O) $(LDLIBS)
	$(DSYM_SCRIPT)

test_asap_ulid: tests/test_asap_ulid.c $(ULID_O) $(CRYPTO_LINK)
	@mkdir -p $(BINDIR)
	$(CC) $(CFLAGS) $(LDFLAGS) $(INC) -pthread -o $(BINDIR)/$@ tests/test_asap_ulid.c $(ULID_O) $(CRYPTO_LINK) -pthread
	$(DSYM_SCRIPT)

test_asap_client: tests/test_asap_client.c $(CLIENT_O) $(ENVELOPE_O) $(ULID_O) $(CRYPTO_LINK) $(CJSON_O) $(PROVIDER_COMMON_O) $(CONFIG_O) $(TOML_O)
	@mkdir -p $(BINDIR)
	$(CC) $(CFLAGS) $(LDFLAGS) $(INC) -pthread -o $(BINDIR)/$@ tests/test_asap_client.c $(CLIENT_O) $(ENVELOPE_O) $(ULID_O) $(CRYPTO_LINK) $(CJSON_O) $(PROVIDER_COMMON_O) $(CONFIG_O) $(TOML_O) $(LDLIBS) -pthread
	$(DSYM_SCRIPT)

test_asap_registry: tests/test_asap_registry.c $(ASAP_REGISTRY_TEST_O) $(CLIENT_O) $(ENVELOPE_O) $(ULID_O) $(CRYPTO_LINK) $(CJSON_O) $(PROVIDER_COMMON_O) $(CONFIG_O) $(TOML_O)
	@mkdir -p $(BINDIR)
	$(CC) $(CFLAGS) $(LDFLAGS) $(INC) -DSHELLCLAW_REGISTRY_TEST -o $(BINDIR)/$@ tests/test_asap_registry.c $(ASAP_REGISTRY_TEST_O) $(CLIENT_O) $(ENVELOPE_O) $(ULID_O) $(CRYPTO_LINK) $(CJSON_O) $(PROVIDER_COMMON_O) $(CONFIG_O) $(TOML_O) $(LDLIBS)
	$(DSYM_SCRIPT)

test_asap_server: tests/test_asap_server.c $(SERVER_O) $(ENVELOPE_O) $(ULID_O) $(CRYPTO_LINK) $(CJSON_O) $(AGENT_O) $(ROUTER_O) $(STUB_O) $(ANTHROPIC_O) $(OPENAI_COMPAT_O) $(OPENAI_O) $(LOCAL_O) $(PROVIDER_COMMON_O) $(CONFIG_O) $(TOML_O) $(MEMORY_O) $(SKILL_O) $(SQLITE3_O)
	@mkdir -p $(BINDIR)
	$(CC) $(CFLAGS) $(LDFLAGS) $(INC) -pthread -o $(BINDIR)/$@ tests/test_asap_server.c $(SERVER_O) $(ENVELOPE_O) $(ULID_O) $(CRYPTO_LINK) $(CJSON_O) $(AGENT_O) $(ROUTER_O) $(STUB_O) $(ANTHROPIC_O) $(OPENAI_COMPAT_O) $(OPENAI_O) $(LOCAL_O) $(PROVIDER_COMMON_O) $(CONFIG_O) $(TOML_O) $(MEMORY_O) $(SKILL_O) $(SQLITE3_O) $(LDLIBS) -pthread
	$(DSYM_SCRIPT)

test_asap_log: tests/test_asap_log.c $(ASAP_LOG_O)
	@mkdir -p $(BINDIR)
	$(CC) $(CFLAGS) $(LDFLAGS) $(INC) -pthread -o $(BINDIR)/$@ tests/test_asap_log.c $(ASAP_LOG_O) -pthread
	$(DSYM_SCRIPT)

test_asap_invoke: tests/test_asap_invoke.c $(ASAP_INVOKE_TEST_O) $(ASAP_REGISTRY_TEST_O) $(CLIENT_O) $(ENVELOPE_O) $(ULID_O) $(CRYPTO_LINK) $(CJSON_O) $(PROVIDER_COMMON_O) $(CONFIG_O) $(TOML_O)
	@mkdir -p $(BINDIR)
	$(CC) $(CFLAGS) $(LDFLAGS) $(INC) -DSHELLCLAW_ASAP_INVOKE_TEST -DSHELLCLAW_REGISTRY_TEST -o $(BINDIR)/$@ tests/test_asap_invoke.c $(ASAP_INVOKE_TEST_O) $(ASAP_REGISTRY_TEST_O) $(CLIENT_O) $(ENVELOPE_O) $(ULID_O) $(CRYPTO_LINK) $(CJSON_O) $(PROVIDER_COMMON_O) $(CONFIG_O) $(TOML_O) $(LDLIBS)
	$(DSYM_SCRIPT)

test_gateway_http: shellclaw tests/test_gateway_http.c $(AUTH_O) $(CONFIG_O) $(TOML_O) $(CJSON_O)
	@if [ "$(GATEWAY)" != "1" ]; then \
		if [ "$(CI)" = "true" ]; then echo "test_gateway_http: GATEWAY=0 in CI — install libwebsockets-dev"; exit 1; fi; \
		echo "test_gateway_http: skipped (GATEWAY=0)"; exit 0; \
	fi
	@mkdir -p $(BINDIR)
	$(CC) $(CFLAGS) $(LDFLAGS) $(INC) -DSHELLCLAW_GATEWAY -pthread -o $(BINDIR)/$@ tests/test_gateway_http.c $(AUTH_O) $(CRYPTO_LINK) $(CONFIG_O) $(TOML_O) $(CJSON_O) $(LDLIBS) -pthread
	$(DSYM_SCRIPT)

test_routes_hardware: tests/test_routes_hardware.c tests/test_routes_json_stub.c $(ROUTES_HARDWARE_O) $(HARDWARE_GPIO_SNAPSHOT_O) $(HARDWARE_TEGRASTATS_O) $(HARDWARE_INIT_O) $(HARDWARE_STUB_O) $(BOARD_DETECT_O) $(HARDWARE_I2C_O) $(HARDWARE_CAMERA_O) $(HARDWARE_LIBGPIOD_O) $(CONFIG_O) $(TOML_O) $(CJSON_O)
	@if [ "$(GATEWAY)" != "1" ]; then \
		if [ "$(CI)" = "true" ]; then echo "test_routes_hardware: GATEWAY=0 in CI — install libwebsockets-dev"; exit 1; fi; \
		echo "test_routes_hardware: skipped (GATEWAY=0)"; exit 0; \
	fi
	@mkdir -p $(BINDIR)
	$(CC) $(CFLAGS) $(LDFLAGS) $(INC) -DSHELLCLAW_GATEWAY -o $(BINDIR)/$@ tests/test_routes_hardware.c tests/test_routes_json_stub.c $(ROUTES_HARDWARE_O) $(HARDWARE_GPIO_SNAPSHOT_O) $(HARDWARE_TEGRASTATS_O) $(HARDWARE_INIT_O) $(HARDWARE_STUB_O) $(BOARD_DETECT_O) $(HARDWARE_I2C_O) $(HARDWARE_CAMERA_O) $(HARDWARE_LIBGPIOD_O) $(CONFIG_O) $(TOML_O) $(CJSON_O) $(LDLIBS) $(LIBGPIOD_LDLIBS) -pthread
	$(DSYM_SCRIPT)

test_static: tests/test_static.c src/gateway/ui_assets.h src/gateway/static.o
	@mkdir -p $(BINDIR)
	./scripts/embed_ui.sh
	$(CC) $(CFLAGS) $(LDFLAGS) $(INC) -o $(BINDIR)/$@ tests/test_static.c src/gateway/static.o $(LDLIBS)
	$(DSYM_SCRIPT)

test_sandbox: tests/test_sandbox.c $(SANDBOX_O)
	@mkdir -p $(BINDIR)
	$(CC) $(CFLAGS) $(LDFLAGS) $(INC) -o $(BINDIR)/$@ tests/test_sandbox.c $(SANDBOX_O) $(LDLIBS)
	$(DSYM_SCRIPT)

test_allowlist: tests/test_allowlist.c $(ALLOWLIST_O)
	@mkdir -p $(BINDIR)
	$(CC) $(CFLAGS) $(LDFLAGS) $(INC) -o $(BINDIR)/$@ tests/test_allowlist.c $(ALLOWLIST_O) $(LDLIBS)
	$(DSYM_SCRIPT)

test_rate_limit: tests/test_rate_limit.c src/gateway/rate_limit.c src/gateway/rate_limit.h
	@mkdir -p $(BINDIR)
	$(CC) $(CFLAGS) $(LDFLAGS) $(INC) -pthread -o $(BINDIR)/$@ tests/test_rate_limit.c src/gateway/rate_limit.c -pthread
	$(DSYM_SCRIPT)

test_asap_http_body: tests/test_asap_http_body.c $(ASAP_HTTP_BODY_O) src/gateway/asap_http_body.h
	@if [ "$(GATEWAY)" != "1" ]; then \
		if [ "$(CI)" = "true" ]; then echo "test_asap_http_body: GATEWAY=0 in CI — install libwebsockets-dev"; exit 1; fi; \
		echo "test_asap_http_body: skipped (GATEWAY=0)"; exit 0; \
	fi
	@mkdir -p $(BINDIR)
	$(CC) $(CFLAGS) $(LDFLAGS) $(INC) $(GATEWAY_CFLAGS) -DSHELLCLAW_GATEWAY -o $(BINDIR)/$@ tests/test_asap_http_body.c $(ASAP_HTTP_BODY_O) $(GATEWAY_LDLIBS)
	$(DSYM_SCRIPT)

test_daemon_smoke: shellclaw
	@chmod +x tests/test_daemon_smoke.sh scripts/install.sh scripts/update.sh
	SHELLCLAW_TEST_BIN="$(BINDIR)/shellclaw" ./tests/test_daemon_smoke.sh

test_bootstrap_keys: shellclaw tests/test_bootstrap_keys.c $(MANIFEST_KEYS_O) $(CRYPTO_O) $(TWEETNACL_O) $(CONFIG_O) $(TOML_O)
	@mkdir -p $(BINDIR)
	$(CC) $(CFLAGS) $(LDFLAGS) $(INC) -o $(BINDIR)/$@ tests/test_bootstrap_keys.c $(MANIFEST_KEYS_O) $(CRYPTO_O) $(TWEETNACL_O) $(CONFIG_O) $(TOML_O) $(LDLIBS)
	$(DSYM_SCRIPT)
	SHELLCLAW_TEST_BIN="$(BINDIR)/shellclaw" $(BINDIR)/$@

test_update_script: shellclaw
	@chmod +x tests/test_update_script.sh scripts/update.sh
	@./tests/test_update_script.sh

test_install_script:
	@chmod +x tests/test_install_script.sh scripts/install.sh
	@./tests/test_install_script.sh

test_download_model:
	@chmod +x tests/test_download_model.sh scripts/download_model.sh
	@./tests/test_download_model.sh

test_hardware_on_device: shellclaw
	@chmod +x tests/test_hardware_on_device.sh
	@SHELLCLAW_TEST_BIN="$(BINDIR)/shellclaw" ./tests/test_hardware_on_device.sh

test_web_dashboard:
	@if [ "$${CI:-}" = "true" ] && ! command -v node >/dev/null 2>&1; then \
		echo "test_web_dashboard: node required when CI=true" >&2; exit 1; \
	fi
	@if command -v node >/dev/null 2>&1; then node tests/test_web_dashboard.js; \
	else echo "test_web_dashboard: skipped (node not installed)"; fi

# Mirrors .github/workflows/ci.yml AddressSanitizer job (CI=true GATEWAY=1).
# -fno-sanitize-recover=all + halt_on_error make any UBSan/ASan finding a hard
# failure (the sanitizer runtime aborts on the first error instead of printing
# a warning and continuing with exit 0). Vendored TweetNaCl UB is suppressed at
# compile time via -fwrapv in TWEETNACL_CFLAGS so the gate only fires on real UB
# in ShellClaw source.
SANITIZE_CFLAGS := -std=c11 -Wall -Wextra -Werror -g -O0 -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all
SANITIZE_LDFLAGS := -fsanitize=address,undefined

test-sanitize:
	$(MAKE) clean
	CI=true GATEWAY=1 CFLAGS="$(SANITIZE_CFLAGS)" LDFLAGS="$(SANITIZE_LDFLAGS)" \
		UBSAN_OPTIONS=halt_on_error=1:abort_on_error=1 \
		ASAN_OPTIONS=halt_on_error=1:abort_on_error=1 \
		$(MAKE) test

bench:
	@chmod +x scripts/bench.sh
	./scripts/bench.sh

static:
	cppcheck --enable=warning,style,performance,portability --error-exitcode=1 \
		-I. -Isrc -Ivendor/tomlc99 -Ivendor/sqlite3 -Ivendor/cJSON \
		--suppress=missingIncludeSystem \
		--suppress=constVariablePointer \
		--suppress=knownConditionTrueFalse \
		--suppress=doubleFree:src/core/config.c \
		--suppress=constParameterPointer \
		--suppress=constParameterCallback \
		--suppress=constParameter:src/hardware/hardware_camera.c \
		--suppress=variableScope:src/hardware/hardware_camera.c \
		--suppress=variableScope:src/vendor/tweetnacl/tweetnacl.c \
		-q src/

test: test_config test_config_patch test_memory test_skill test_provider test_anthropic test_openai test_local_provider test_router test_heartbeat test_agent test_reload test_channel test_cli test_shell test_file test_telegram test_discord_helpers test_web_search test_cron test_context test_dispatch test_crypto test_hardware_stub test_board_detect test_hardware_libgpiod test_hardware_i2c test_hardware_camera test_pin_tables test_hardware_init test_hardware_gpio_snapshot test_hardware_tegrastats test_hardware_tools test_registry test_ws test_manifest $(ASAP_UNIT_TESTS) test_sandbox test_allowlist test_rate_limit test_daemon_smoke test_bootstrap_keys test_update_script test_install_script test_download_model test_web_dashboard test_routes_hardware
	$(BINDIR)/test_config
	$(BINDIR)/test_config_patch
	$(BINDIR)/test_memory
	$(BINDIR)/test_skill
	$(BINDIR)/test_provider
	$(BINDIR)/test_anthropic
	$(BINDIR)/test_openai
	$(BINDIR)/test_local_provider
	$(BINDIR)/test_router
	$(BINDIR)/test_heartbeat
	$(BINDIR)/test_agent
	$(BINDIR)/test_reload
	$(BINDIR)/test_channel
	$(BINDIR)/test_cli
	$(BINDIR)/test_shell
	$(BINDIR)/test_file
	$(BINDIR)/test_telegram
	$(BINDIR)/test_discord_helpers
	$(BINDIR)/test_web_search
	$(BINDIR)/test_cron
	$(BINDIR)/test_context
	$(BINDIR)/test_dispatch
	$(BINDIR)/test_crypto
	$(BINDIR)/test_hardware_stub
	$(BINDIR)/test_board_detect
	$(BINDIR)/test_hardware_libgpiod
	$(BINDIR)/test_hardware_i2c
	$(BINDIR)/test_hardware_camera
	$(BINDIR)/test_pin_tables
	$(BINDIR)/test_hardware_init
	$(BINDIR)/test_hardware_gpio_snapshot
	$(BINDIR)/test_hardware_tegrastats
	$(BINDIR)/test_hardware_tools
	$(BINDIR)/test_registry
	$(BINDIR)/test_ws
	@for t in $(ASAP_UNIT_TESTS); do $(BINDIR)/$$t || exit 1; done
	$(BINDIR)/test_sandbox
	$(BINDIR)/test_allowlist
	$(BINDIR)/test_rate_limit
	$(MAKE) test_install_script
	$(MAKE) test_download_model
	@if [ "$(SHELLCLAW_HW_TEST)" = "1" ]; then $(MAKE) test_hardware_on_device; fi
	$(MAKE) test_web_dashboard
	$(MAKE) test_auth && $(BINDIR)/test_auth
	$(MAKE) test_static && $(BINDIR)/test_static
	@if [ "$(GATEWAY)" = "1" ]; then \
		$(BINDIR)/test_routes_hardware && \
		$(MAKE) test_asap_http_body && $(BINDIR)/test_asap_http_body && \
		$(MAKE) test_gateway_http && $(BINDIR)/test_gateway_http; \
	elif [ "$(CI)" = "true" ]; then echo "GATEWAY=0 in CI — install libwebsockets-dev"; exit 1; fi

COVERAGE_DIR := build/coverage
COVERAGE_MIN := 80

coverage: clean
	$(MAKE) BUILD=coverage GATEWAY=0 test_config test_config_patch test_memory test_skill test_provider test_anthropic test_openai test_local_provider test_router test_heartbeat test_agent test_reload test_channel test_cli test_shell test_file test_telegram test_discord_helpers test_web_search test_cron test_context test_dispatch test_crypto test_hardware_stub test_board_detect test_hardware_libgpiod test_hardware_i2c test_hardware_camera test_pin_tables test_hardware_init test_hardware_gpio_snapshot test_hardware_tegrastats test_hardware_tools test_registry test_ws test_manifest_build test_manifest_keys test_jcs $(ASAP_UNIT_TESTS) test_sandbox test_allowlist test_rate_limit test_auth

	@if [ "$(GATEWAY)" = "1" ]; then $(MAKE) BUILD=coverage GATEWAY=1 shellclaw test_gateway_http test_static; fi
	@chmod +x scripts/coverage.sh
	@BINDIR=$(BINDIR) COVERAGE_DIR=$(COVERAGE_DIR) COVERAGE_MIN=$(COVERAGE_MIN) GATEWAY=$(GATEWAY) ./scripts/coverage.sh

# Remove build artifacts left in repo root by old Makefiles (binaries and .dSYM)
clean-root-dsym:
	@for d in shellclaw test_agent test_anthropic test_channel test_cli test_config test_file test_memory test_local_provider test_openai test_provider test_router test_shell test_skill test_telegram test_web_search test_ws; do rm -rf $$d.dSYM; done
	@rm -f shellclaw test_agent test_anthropic test_channel test_cli test_config test_file test_memory test_local_provider test_openai test_provider test_router test_shell test_skill test_telegram test_web_search test_ws

clean: clean-root-dsym
	rm -f $(OBJS) $(PROVIDER_COMMON_O) $(STUB_O) $(ANTHROPIC_O) $(OPENAI_COMPAT_O) $(OPENAI_O) $(LOCAL_O) $(ROUTER_O) $(CJSON_O) $(TWEETNACL_O) $(ANTHROPIC_TEST_O) $(OPENAI_TEST_O) $(LOCAL_TEST_O) $(CONTEXT_TEST_OBJS) $(HEARTBEAT_TEST_O) $(CHANNEL_TG_TEST_O) $(CHANNEL_COMMON_O) $(CHANNEL_STUB_O) $(CHANNEL_CLI_O) $(CHANNEL_TG_O) $(CHANNEL_DISCORD_O) $(DISCORD_HELPERS_O) $(CHANNEL_HEARTBEAT_O) $(CHANNEL_WEBCHAT_O) $(AUTH_O) $(STATIC_O) $(HTTP_O) $(HTTP_LWS_O) $(ASAP_HTTP_BODY_O) $(ROUTES_O) $(ROUTES_HARDWARE_O) $(WS_O) $(MANIFEST_O) $(MANIFEST_PROFILES_O) $(MANIFEST_BUILD_O) $(MANIFEST_SIGN_O) $(MANIFEST_KEYS_O) $(ENVELOPE_O) $(ULID_O) $(CLIENT_O) $(ASAP_REGISTRY_O) $(SERVER_O) $(ASAP_LOG_O) $(RATE_LIMIT_O) $(SHELL_O) $(WEBSEARCH_O) $(FILE_O) $(REGISTRY_O) $(CONTEXT_O) $(CONTEXT_CACHE_O) $(CONTEXT_HTTP_O) $(CONTEXT_GEO_O) $(CRYPTO_O) $(JCS_O) $(HARDWARE_STUB_O) $(HARDWARE_INIT_O) $(HARDWARE_GPIO_SNAPSHOT_O) $(HARDWARE_TEGRASTATS_O) $(HARDWARE_TOOLS_O) $(BOARD_DETECT_O) src/hardware/hardware_libgpiod.o $(HARDWARE_I2C_O) $(HARDWARE_CAMERA_O) $(CRON_O) $(ASAP_INVOKE_O) $(SANDBOX_CORE_O) $(SANDBOX_LANDLOCK_O) $(ALLOWLIST_SCAN_O) $(ALLOWLIST_PATH_O)
	rm -f src/gateway/ui_assets.h
	find . -name '*.gcno' -o -name '*.gcda' -o -name '*.gcov' | xargs rm -f 2>/dev/null || true
	rm -f $(WS_TEST_O) $(BINDIR)/asap_registry_test.o $(BINDIR)/asap_invoke_test.o $(CONTEXT_TEST_OBJS) $(HEARTBEAT_TEST_O) $(BINDIR)/shellclaw $(BINDIR)/test_tweetnacl_smoke $(BINDIR)/test_config $(BINDIR)/test_config_patch $(BINDIR)/test_memory $(BINDIR)/test_skill $(BINDIR)/test_provider $(BINDIR)/test_anthropic $(BINDIR)/test_openai $(BINDIR)/test_local_provider $(BINDIR)/test_router $(BINDIR)/test_heartbeat $(BINDIR)/test_crypto $(BINDIR)/test_hardware_stub $(BINDIR)/test_board_detect $(BINDIR)/test_hardware_libgpiod $(BINDIR)/test_hardware_i2c $(BINDIR)/test_hardware_camera $(BINDIR)/test_pin_tables $(BINDIR)/test_hardware_init $(BINDIR)/test_hardware_tools $(BINDIR)/test_registry $(BINDIR)/test_ws $(BINDIR)/test_agent $(BINDIR)/test_channel $(BINDIR)/test_cli $(BINDIR)/test_shell $(BINDIR)/test_file $(BINDIR)/test_telegram $(BINDIR)/test_discord_helpers $(BINDIR)/test_web_search $(BINDIR)/test_cron $(BINDIR)/test_context $(BINDIR)/test_manifest_build $(BINDIR)/test_manifest_keys $(BINDIR)/test_jcs $(BINDIR)/test_asap_envelope $(BINDIR)/test_asap_ulid $(BINDIR)/test_asap_client $(BINDIR)/test_asap_registry $(BINDIR)/test_asap_server $(BINDIR)/test_asap_invoke $(BINDIR)/test_asap_log $(BINDIR)/test_auth $(BINDIR)/test_gateway_http $(BINDIR)/test_static $(BINDIR)/test_sandbox $(BINDIR)/test_allowlist $(BINDIR)/test_rate_limit
	rm -rf $(BINDIR)/*.dSYM $(DSYMDIR)
	rm -f $(BOOTSTRAP_DISPATCH_STUB_O) $(TOOL_RELOAD_STUB_O) $(RELOAD_CHANNEL_STUB_O) $(HTTP_RELOAD_STUB_O)
