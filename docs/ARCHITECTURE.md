# ShellClaw architecture

Distilled product architecture for ShellClaw v1.0 — one `aarch64` C binary that scales from Raspberry Pi Zero 2 W to NVIDIA Jetson Orin Nano Super with runtime board detection. For build commands and the high-level diagram, see [README.md](../README.md). For module-level coding rules, see [AGENTS.md](../AGENTS.md).

---

## Design goals

| Constraint | Target | As-built (v1.0) |
|------------|--------|-----------------|
| Release binary | < 2 MB (CI gate) | < 600 KB with hardware backends |
| Agent RAM | < 5 MB idle, < 15 MB active | Target; Jetson on-device measurement is [known pending](JETSON_SIGNOFF.md) |
| Startup | < 1 s Jetson, < 2 s RPi | Board-dependent; see [BENCHMARKS.md](BENCHMARKS.md) when published |
| Language | C99/C11 | Single tree, no runtime interpreter |
| Hardware | GPIO + I2C + camera abstraction | GPIO + I2C live in v1.0; sensor decoders + camera image return in v1.2 |

**Dual persona, one binary** ([DR-015](https://github.com/asap-protocol/shellclaw/blob/main/.cursor/strategy/decision-records/decisions.md)): the agent reads `/proc/device-tree/compatible` at startup (`src/hardware/board_detect.c`) and selects Jetson vs RPi backends. Override with `SHELLCLAW_BOARD=jetson|rpi|stub` for tests.

---

## System diagram

```
Channels (CLI, Telegram, Discord, WebChat)
         │
         ▼
   ┌──────────────┐     ┌─────────────────────────────┐
   │  Agent Loop  │────►│  LLM providers (router)    │
   │  (ReAct)     │     │  Anthropic · OpenAI · local │
   │              │◄────│  (llama-server subprocess)  │
   └──────┬───────┘     └─────────────────────────────┘
          │
   ┌──────▼───────┐     ┌─────────────────────────────┐
   │  Tools       │────►│  Hardware (per-board)       │
   │  shell,file, │     │  GPIO · I2C · camera CLI    │
   │  search,cron,│     │  (libgpiod + i2c-dev)       │
   │  asap_invoke │     └─────────────────────────────┘
   └──────┬───────┘
          │
          │           ┌─────────────────────────────┐
          └──────────►│  Gateway (libwebsockets)    │
                      │  HTTP · WebSocket · Web UI  │
                      └──────────────┬──────────────┘
                                     │
                      ┌──────────────▼──────────────┐
                      │  ASAP (manifest, /asap, log) │
                      │  Registry (read-only fetch)  │
                      └─────────────────────────────┘
```

Shell commands run in a **Linux sandbox** (namespaces + cgroups v2). Hardware tools run in the main agent process — they are **not** exposed inside the sandboxed shell namespace. See [SECURITY.md](SECURITY.md).

---

## Module map (`src/`)

| Directory | Responsibility | Key entry points |
|-----------|----------------|------------------|
| `core/` | Agent loop, TOML config, SQLite memory/sessions, skills, daemon bootstrap | `agent_run()`, `config_load()`, `init_subsystems()` |
| `providers/` | LLM backends and fallback router | `provider_router_chat()`, `provider_local_get()` |
| `tools/` | Agent-callable tools | `shell`, `file`, `web_search`, `asap_invoke`, `cron`, `context`, hardware GPIO/I2C/camera |
| `channels/` | Inbound/outbound I/O | CLI, Telegram, Discord, WebChat, heartbeat |
| `gateway/` | Embedded HTTP/WebSocket server, pairing auth, rate limits, static Web UI | `http_lws`, `routes.c`, `routes_hardware.c` |
| `asap/` | Protocol client/server, envelope, ULID, registry cache, signed manifest | `manifest_build_signed_json()`, `POST /asap` |
| `sandbox/` | Process isolation for shell tool | `sandbox_exec()` — user ns + `unshare(CLONE_NEWNS\|NEWNET\|NEWPID)` then fork for PID 1, Landlock workspace bound, no `pivot_root` |
| `hardware/` | Board abstraction: GPIO (libgpiod), I2C (`/dev/i2c-N`), camera (fixed-argv CLI spawn) | `hardware_init()`, `board_detect()` |
| `crypto/` | Ed25519 signing + JCS canonicalization for manifests | `manifest_keys_ensure_loaded()` (lazy on manifest GET), `jcs.c` |

Convention: one primary `.c` + `.h` per module; new tools go in `src/tools/<name>.c` with matching `tests/test_<name>.c`.

---

## Agent loop (ReAct)

1. **Channels** deliver user text (or cron/heartbeat triggers) into `agent_run()`.
2. **Skills** from `~/.shellclaw/skills/` (hot-reload optional) extend the system prompt.
3. **Memory** recalls recent SQLite entries; sessions hold conversation JSON.
4. **Router** walks `providers.fallback_chain` (e.g. `anthropic` → `local` → `stub`) on transport/5xx errors; 4xx stops the chain.
5. **Tools** execute when the model returns tool calls; results feed the next iteration until a final reply or `max_tool_iterations`.

**Thread safety:** the main loop is single-threaded. Gateway worker threads (WebSocket chat, `POST /asap`) must hold `agent_lock()` around `agent_run()`, inbound `mcp.tool_call` execute, and `state.query` memory-store reads. See README § Thread Safety.

---

## Gateway and Web UI

- Binds `[gateway] host` / `port` (default `127.0.0.1:18789`).
- **Auth:** pairing token; `/api/*` requires Bearer except `/health`, `/pair`, `/.well-known/*`.
- **WebSocket:** browsers use subprotocol `bearer.<token>` (not `Authorization` header).
- **Hardware API:** `/api/hardware/*` — board info, GPIO snapshot, tegrastats on Jetson; sensor/camera panels stubbed “Coming in v1.2”.
- **Rate limits:** per-IP on `/asap`; per-token on camera snapshot (1/sec) when enabled.
- **Signed manifest:** `GET /.well-known/asap/manifest.json` builds and signs the JSON synchronously in the HTTP handler (single-threaded today; not safe for concurrent manifest builds without locking).

Static assets are embedded at build time (`scripts/embed_ui.sh`).

---

## Hardware abstraction

| Layer | Jetson Orin Nano Super | Raspberry Pi Zero 2 W |
|-------|------------------------|------------------------|
| Detection | `nvidia,p3768` / `tegra234` in device tree | `raspberrypi,model-zero-2-w` |
| GPIO | libgpiod on `gpiochip0` (`tegra234-gpio`) | libgpiod on `gpiochip0` (`bcm2835-gpio`) |
| Pin map | `src/hardware/boards/jetson_orin_nano.h` | `src/hardware/boards/rpi_zero2w.h` |
| Default I2C bus | 7 (`/dev/i2c-7`, header I2C1) | 1 |
| Camera CLI | CSI: `gst-launch-1.0` + `nvarguscamerasrc`; USB: `v4l2-ctl` | CSI: `libcamera-still` |

**v1.0 scope:** `gpio_read` / `gpio_write` / `gpio_mode`, `i2c_scan` / `i2c_read` / `i2c_write`, camera capture CLI skeleton. **v1.2:** BME280/BH1750 decoders, DHT22 (experimental), multimodal camera image return to the LLM. See [HARDWARE_JETSON.md](HARDWARE_JETSON.md).

SFIO pins (I2C/UART/SPI) are rejected by GPIO tools with a pinmux error — use only pins marked GPIO in the board pin table.

---

## Local inference

ShellClaw does **not** embed llama.cpp. A separate **`llama-server`** process serves OpenAI-compatible chat completions; `providers/local.c` probes `GET /health` and `GET /v1/models` at startup.

- **Jetson:** CUDA build via `scripts/build_llama_jetson.sh`; systemd unit `llama-server.service` + `/etc/shellclaw/llama-server.env`.
- **RPi:** CPU build via `scripts/build_llama_rpi.sh` (validated in Phase 6).

Details: [LOCAL_INFERENCE.md](LOCAL_INFERENCE.md).

---

## ASAP integration

- **Signed manifest** at `GET /.well-known/asap/manifest.json` when gateway + keys are enabled.
- **Public URL (Q-URL):** GitHub Pages publishes release manifests — see [ASAP.md](ASAP.md).
- **Registry:** read-only fetch of upstream `registry.json`; `asap_invoke` tool calls peer agents.
- **Keys:** `~/.shellclaw/keys/ed25519.{priv,pub}` mode `0600`; agent refuses startup on loose permissions.

v1.0 ships static manifest discovery; live cross-agent HTTP and full compliance harness green run are v1.0.1+.

---

## Sandbox

Linux path (`src/sandbox/sandbox.c`):

- Isolator: user namespace when unprivileged, then `unshare(CLONE_NEWNS | CLONE_NEWNET | CLONE_NEWPID)`, then **fork** so the command is PID 1.
- Isolation failure is fail-closed via a control pipe (not `sh` exit 122/123).
- Landlock workspace bound when `workspace_path` is set (RW workspace, RO `/bin` `/usr` `/lib*`, RW `/dev/null`).
- **No** `pivot_root()` — GPU device nodes are not granted by Landlock; the allowlist still blocks `/dev/nv*` literals.
- Allowlist in `src/sandbox/allowlist.c` is defense-in-depth (quoted paths, `$HOME`/`$PWD`, `file:` URLs).
- cgroups v2: `memory.max`, `cpu.max` when writable under `/sys/fs/cgroup`.

Non-Linux: plain `fork`/`exec` with a stderr warning.

---

## Configuration and secrets

- Primary config: `~/.shellclaw/config.toml` (see `config.example.toml`).
- API keys via environment variables referenced in config (`api_key_env`).
- Hardware block: `[hardware] enabled`, optional `board`, `gpio_test_pin`, `i2c_bus`, camera defaults.
- ASAP block: `[asap] public_base_url`, `agent_urn`, registry URL.

Never commit credentials; use `.env.example` as reference.

---

## As-built notes vs original plan

| Planned | v1.0 as-built |
|---------|---------------|
| Full sensor + camera UX | Deferred to v1.2 (Phase 7); Web UI placeholders |
| RPi primary validation | Jetson-first in v1.0; RPi in v1.1 |
| External security audit | Self-audit only ([SECURITY.md](SECURITY.md) Limitations) |
| Green ASAP compliance harness | Documented deviations; static manifest registration |
| `PLAN.md` §5–6 in repo | Architecture lives here + `.cursor/rules/shellclaw-architecture.mdc` |

---

## Related docs

| Doc | Topic |
|-----|-------|
| [HARDWARE_JETSON.md](HARDWARE_JETSON.md) | JetPack, NVMe, pins, wiring |
| [HARDWARE_SAFETY.md](HARDWARE_SAFETY.md) | 3V3 logic, ESD (Jetson + RPi) |
| [LOCAL_INFERENCE.md](LOCAL_INFERENCE.md) | llama.cpp, models, tegrastats |
| [ASAP.md](ASAP.md) | Manifest, marketplace, compliance harness |
| [SECURITY.md](SECURITY.md) | Threat model, sandbox audit |
