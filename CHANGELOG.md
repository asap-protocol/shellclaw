# Changelog

All notable changes to ShellClaw are documented here. Format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added
- Phase 5 documentation suite (`docs/SECURITY.md`, `docs/ASAP.md`, and related guides).
- `CONTRIBUTING.md` with PR workflow and pre-tag `gpio-mockup` ritual.
- Jetson-aware `[hardware]` defaults in `config.example.toml` and `.env.example`.

### Changed
- `main` is the active line. On-device Jetson sign-off is a known pending item, not a merge gate ([`docs/JETSON_SIGNOFF.md`](docs/JETSON_SIGNOFF.md)).
- Gateway `/health` `version` matches `SHELLCLAW_RELEASE_VERSION`.

### Security
- `auth_pair` fails closed when bearer RNG fails (no uninitialized token, no tokens-file write, pairing code kept).
- Camera auto-output keeps the exclusive `mkstemp` inode (no unlink + `${tmpl}.jpg` sibling).
- Reject I2C `bus` outside 0–255 at the tool JSON boundary.

---

## [1.0.0] - TBD

**Phase 5: Edge AI Hardware & Release** — Jetson Orin Nano Super primary target.

### Added
- Hardware abstraction: GPIO (libgpiod), I2C scan, camera CLI skeleton with per-board backends and runtime board detection (`/proc/device-tree/compatible`, `SHELLCLAW_BOARD` override).
- Jetson-specific: `tegrastats` GPU metrics parser, 40-pin GPIO snapshot, `/hardware` Web UI and `/api/hardware/*` REST routes (Bearer auth; camera HTTP capture deferred).
- CUDA-accelerated local inference path: `scripts/build_llama_jetson.sh`, `scripts/download_model.sh` (Phi-3-mini Q4_K_M default), systemd units for `llama-server` + `shellclaw`.
- Ed25519 manifest signing (`src/crypto/`, TweetNaCl), JCS canonicalization, strict key file permissions (0600), fail-fast startup on loose keys.
- Board-aware ASAP manifest capabilities (hardware class/model, local model id, GPIO/I2C tools).
- ASAP marketplace registration workflow and static manifest on GitHub Pages ([`docs/ASAP.md`](docs/ASAP.md)).
- Security self-audit: sandbox GPU/Argus blocklist, camera argv-only spawn, gateway hardware auth review ([`docs/SECURITY.md`](docs/SECURITY.md)).
- `make test-sanitize` (AddressSanitizer + UBSan) wired into CI.
- Example skills for v1.0: `assistant`, `edge-briefing`, `server-admin` (sensor/camera skills deferred to v1.2).

### Changed
- README dual-target positioning (Jetson edge-AI + RPi hobbyist) and Phase 7 roadmap for deferred physical-world features.

### Security
- Blocklist Jetson GPU `/dev` nodes and `/tmp/argus_socket` from sandboxed shell.
- `/api/hardware/camera/snapshot` is a v1.2 deferred stub; per-token 1 req/s throttle is not shipped until Phase 7 HTTP capture ([`docs/SECURITY.md`](docs/SECURITY.md)).

### Known pending (not a v1.2 deferral)
- On-device Jetson Orin Nano Super sign-off: GPIO/I2C/`llama-server` smoke, benchmark fill ([`docs/JETSON_SIGNOFF.md`](docs/JETSON_SIGNOFF.md)). Not a merge-to-`main` gate.

### Deferred to v1.2 (Phase 7)
- BME280, BH1750, DHT22 sensor decoders and Web UI sensor panels.
- CSI/USB camera image return path for multimodal LLMs.
- `home-monitor` and `visual-monitor` skills.

---

## [0.4.0]

**Phase 4: Autonomy**

### Added
- Local inference provider (`llama-server` subprocess, CPU profile for dev/RPi prep).
- Provider fallback chain and autonomy dashboard in Web UI.
- Discord channel, systemd install/update scripts, OTA update flow.
- Context tool (geolocation-aware), cron scheduler enhancements.

### Changed
- Gateway routes split; WebSocket Bearer auth via subprotocol `bearer.<token>` (breaking vs early gateway builds).

---

## [0.3.0]

**Phase 3: Protocol**

### Added
- ASAP Protocol client/server, envelope parsing, ULID, registry client.
- `asap_invoke` tool, `/asap` endpoint, `/api/asap/log`.
- Linux sandbox: namespaces + cgroups v2, command allowlist.
- Tavily web search provider, gateway rate limits.

---

## [0.2.0]

**Phase 2: Gateway**

### Added
- Embedded HTTP server and Web UI (libwebsockets).
- WebSocket chat, pairing auth, bearer tokens.
- Cron scheduler, skill hot-reload, ASAP manifest stub endpoint.

---

## [0.1.0]

**Phase 1: Foundation**

### Added
- Core ReAct agent loop, SQLite memory and sessions.
- CLI and Telegram channels; Anthropic and OpenAI providers.
- Shell, file, and web search tools; skill loading from markdown.

[Unreleased]: https://github.com/asap-protocol/shellclaw/compare/v1.0.0...HEAD
[1.0.0]: https://github.com/asap-protocol/shellclaw/compare/v0.4.0...v1.0.0
[0.4.0]: https://github.com/asap-protocol/shellclaw/compare/v0.3.0...v0.4.0
[0.3.0]: https://github.com/asap-protocol/shellclaw/compare/v0.2.0...v0.3.0
[0.2.0]: https://github.com/asap-protocol/shellclaw/compare/v0.1.0...v0.2.0
[0.1.0]: https://github.com/asap-protocol/shellclaw/releases/tag/v0.1.0
