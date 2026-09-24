# ShellClaw

**The first physical AI agent in a global agent ecosystem — and the only one that scales from a $15 Raspberry Pi to a $249 NVIDIA Jetson with the same C-native binary.**

A lightweight AI assistant written in C that runs on **NVIDIA Jetson Orin Nano Super** (8 GB, 67 TOPS, CUDA-accelerated local LLM) for the edge-AI maker / researcher persona, and on **Raspberry Pi Zero 2 W** (~$15, smallest viable Linux SBC) for the hobbyist persona — from a single `aarch64` binary with runtime board detection. It communicates with other agents through the [agentic marketplace](https://asap-protocol.com/browse), using [ASAP Protocol](https://github.com/asap-protocol/asap-protocol).

**Two personas, one binary** (see [DR-015](.cursor/strategy/decision-records/decisions.md), [DR-016](.cursor/strategy/decision-records/decisions.md)):

| Persona | Hardware | Headline capability (final form, fully delivered in v1.2) |
|---|---|---|
| Edge-AI maker / researcher | Jetson Orin Nano Super 8 GB Dev Kit | Local Phi-3-mini Q4 @ 25–35 tok/s or Llama-3.1-8B Q4 @ 14–18 tok/s via CUDA; GPIO + I2C + CSI/USB camera; NVMe boot. CUDA inference + GPIO ships in v1.0; sensors + camera image return in v1.2 |
| Hobbyist / IoT tinkerer | Raspberry Pi Zero 2 W | < 5 MB RAM, < 500 KB binary on the cheapest viable Linux SBC; GPIO + I2C + CSI/USB camera; cloud LLM primary with TinyLlama 1.1B CPU emergency fallback. Same binary, RPi-validated in v1.1; sensors + camera in v1.2 |

**Roadmap (high level):**

| Phase | Version | Status | Focus |
|-------|---------|--------|-------|
| 1: Foundation | v0.1.0 | ✅ Done | Core agent loop, CLI + Telegram, Anthropic/OpenAI, shell/search/file tools, SQLite memory & sessions, skill loading |
| 2: Gateway | v0.2.0 | ✅ Done | HTTP server, embedded Web UI, WebSocket chat, cron scheduler, pairing auth, ASAP manifest, skill hot-reload |
| 3: Protocol | v0.3.0 | ✅ Done | ASAP client/server, registry, `asap_invoke` tool, process sandbox (namespaces + cgroups), Tavily search, `/asap` + `/api/asap/log`, rate limits |
| 4: Autonomy | v0.4.0 | ✅ Done | Local inference (llama.cpp), provider fallback, Discord channel, systemd service, OTA updates, context tool, dashboard |
| 5: Edge AI Hardware & Release | v1.0.0 | Landed (Jetson on-device pending) | Hardware abstraction (GPIO, I2C, camera CLI skeleton), CUDA local LLM path, Ed25519 signing, ASAP marketplace docs. **Known pending:** physical Jetson sign-off — [`docs/JETSON_SIGNOFF.md`](docs/JETSON_SIGNOFF.md) |
| 6: Hobbyist Portability | v1.1.0 | — | **Raspberry Pi Zero 2 W validation**: same binary, RPi-specific install + CPU-only local LLM (TinyLlama 1.1B) + benchmarks + docs, optional pre-built SD image |
| 7: Physical World Hardware | v1.2.0 | — | **Real sensors + camera image return** on both boards: BME280, BH1750, DHT22 (experimental), CSI + USB camera capture, Web UI sensor/camera panels, `home-monitor` + `visual-monitor` skills |

*v1.2 (Phase 7) intentionally deferred from v1.0:* sensor decoders (BME280, BH1750, DHT22), CSI/USB camera image return to the LLM, Hardware Web UI sensor/camera tabs (currently "Coming in v1.2"), and the `home-monitor` / `visual-monitor` skills. GPIO, I2C scan, CUDA inference, and the camera CLI skeleton ship in v1.0.

*Known pending:* on-device validation on a physical Jetson Orin Nano Super is **not** a merge gate. See [`docs/JETSON_SIGNOFF.md`](docs/JETSON_SIGNOFF.md).

## What makes ShellClaw different

ShellClaw is **not another OpenClaw clone** in a different language. It is a **hardware-native, dual-persona agent**: it interacts with the physical world (GPIO, I2C sensors, camera) on both ends of the SBC spectrum, runs production-grade local LLMs on edge-AI hardware (CUDA on Jetson), and collaborates with cloud agents through a standardized protocol — all from a single C source tree and a single `aarch64` binary.

| Feature | ShellClaw |
|---|---|
| **Binary** | < 500 KB base, < 600 KB with hardware backends |
| **Agent RAM** | < 5 MB idle, < 15 MB active (on both boards) |
| **Startup** | < 1 s on Jetson, < 2 s on RPi Zero 2 W |
| **Language** | C (~5,500 lines target after Phase 5) |
| **Hardware** | GPIO, I2C, CSI/USB Camera (single abstraction, per-board backends) |
| **Sandbox** | Native Linux namespaces + cgroups v2 |
| **Web UI** | Embedded in binary |
| **Local inference** | CUDA `llama-server` on Jetson (14–35 tok/s); CPU `llama-server` on RPi (emergency) |
| **Agent network** | ASAP Protocol (first non-Python + first edge-AI ASAP agent) |
| **Hardware range** | Same binary runs on a $15 RPi Zero 2 W and a $249 Jetson Orin Nano Super |

## Build and run

**Build:** `make shellclaw` → binary at `build/shellclaw`. `make test` → builds and runs all tests in `build/`.

**Run:** `./build/shellclaw`

**Quality checks:**
- `make static` — cppcheck on `src/` (requires cppcheck)
- `make test-sanitize` — AddressSanitizer + UBSan full suite
- `make coverage` — coverage report; fails if core < 80% (requires lcov)
- CI enforces release binary < 2 MB; optional `asap-compliance` when the Python package is available
- **Before opening a PR:** run `CI=true make clean && CI=true make test` (matches Linux CI with `-Werror`), or on a machine with the same apt deps as [.github/workflows/ci.yml](.github/workflows/ci.yml): `chmod +x scripts/ci-local.sh && ./scripts/ci-local.sh`

See [CONTRIBUTING.md](CONTRIBUTING.md) for PR workflow, coding standards, and the pre-tag `gpio-mockup` release ritual.

**Configuration:** copy [`config.example.toml`](config.example.toml) to `~/.shellclaw/config.toml` and [`.env.example`](.env.example) to `.env`. Phase 3+ keys (ASAP, sandbox, gateway) and the Jetson `[hardware]` block are documented there. Install **libwebsockets** (`pkg-config` must find it) to build the gateway and run `GATEWAY=1 make test_gateway_http`.

**Jetson install (v1.0):** `./scripts/install.sh`, `./scripts/build_llama_jetson.sh`, `./scripts/download_model.sh phi3` — details in [`docs/HARDWARE_JETSON.md`](docs/HARDWARE_JETSON.md) and [`docs/LOCAL_INFERENCE.md`](docs/LOCAL_INFERENCE.md).

**WebSocket auth (breaking vs early gateway builds):** browsers cannot send `Authorization` on WebSocket; use subprotocol `bearer.<pairing-token>` when opening `/ws` (see `web/js/app.js`).

**Debug (macOS):** Symbols in `tests-dSYM/`. Use `lldb build/test_agent` then `settings set target.debug-file-search-path tests-dSYM`. Old `.dSYM` in repo root? Run `make clean-root-dsym`.

## Documentation

| Doc | Contents |
|-----|----------|
| [CONTRIBUTING.md](CONTRIBUTING.md) | PR process, coding standards, pre-tag rituals |
| [CHANGELOG.md](CHANGELOG.md) | Release history v0.1.0 → v1.0.0 |
| [AGENTS.md](AGENTS.md) | Agent/coder quickstart |
| [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) | As-built module map and data flow |
| [docs/JETSON_SIGNOFF.md](docs/JETSON_SIGNOFF.md) | On-device Jetson checklist (**known pending**, not a merge gate) |
| [docs/HARDWARE_SAFETY.md](docs/HARDWARE_SAFETY.md) | 3V3 logic, current limits, ESD |
| [docs/SECURITY.md](docs/SECURITY.md) | Threat model, sandbox audit, gateway auth |
| [docs/ASAP.md](docs/ASAP.md) | Manifest signing, marketplace registration |
| [docs/LOCAL_INFERENCE.md](docs/LOCAL_INFERENCE.md) | llama.cpp build, models, memory budgeting |
| [docs/BENCHMARKS.md](docs/BENCHMARKS.md) | Jetson + x86 performance numbers |

## Thread Safety

The **main agent loop** is single-threaded: memory, providers, channels and tools keep much of their state in process-wide data initialized at startup. **Inbound HTTP/WebSocket paths** (for example ASAP `POST /asap` and the WebSocket chat dispatcher) may run on **libwebsockets worker threads**. 

Those code paths must call `agent_lock()` / `agent_unlock()` around `agent_run()`, inbound ASAP `mcp.tool_call` execute, and `state.query` memory-store reads so only one thread uses shared session/memory state at a time. Do not call `agent_run`, provider `chat`, or memory functions from arbitrary new threads without the same discipline.

## Architecture

```
Channels (CLI, Telegram, Discord, WebChat)
         │
         ▼
   ┌──────────────┐     ┌─────────────────────────────┐
   │  Gateway     │────►│  Embedded Web UI + REST     │
   │  HTTP / WS   │     │  /hardware · /asap · auth   │
   └──────┬───────┘     └─────────────────────────────┘
          │ agent_lock()
          ▼
   ┌──────────────┐     ┌─────────────────────────────┐
   │  Agent Loop  │────►│  LLM providers              │
   │  (ReAct)     │     │  Anthropic · OpenAI · local │
   │              │◄────│  (llama-server: CUDA Jetson │
   │              │     │   / CPU RPi)                │
   └──────┬───────┘     └─────────────────────────────┘
          │
   ┌──────▼───────┐     ┌──────────┐  ┌──────────────┐
   │  Tools       │────►│ Sandbox  │  │  Hardware    │
   │  shell·file  │     │ shell ns │  │ GPIO·I2C·cam │
   │  search·cron │     │ + cgroup │  │ libgpiod +   │
   │  asap·context│     └──────────┘  │ board detect │
   └──────┬───────┘                   └──────────────┘
          │
          │           ┌─────────────────────────────┐
          └──────────►│  ASAP + crypto              │
                      │  Ed25519 manifest · registry│
                      │  envelope · peer invoke     │
                      └─────────────────────────────┘
```

**One source tree, one `aarch64` binary, two hardware personas.** At startup the agent reads `/proc/device-tree/compatible` (or `SHELLCLAW_BOARD`) and selects backends — Jetson `tegra234-gpio` / `nvarguscamerasrc` vs RPi `bcm2835-gpio` / `libcamera-still`. Module layout: `src/core`, `providers`, `tools`, `channels`, `gateway`, `asap`, `sandbox`, `hardware`, `crypto` — see [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md).

## License

[MIT](LICENSE) — permissive, simple and aligned with the ASAP ecosystem and similar agents. Use, modify and distribute freely; keep the copyright notice.
