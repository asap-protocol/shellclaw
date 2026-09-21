# Changelog

All notable changes to ShellClaw are documented here. Format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Fixed
- `write_file` maps to the intended path instead of the first existing ancestor, so a nested path cannot truncate a workspace file treated as a directory or overwrite a same-named file in a parent (#67). Dangling workspace symlinks are rejected (`lstat` + `O_NOFOLLOW`) instead of creating host files outside the workspace (#90).
- Discord Gateway RX grows for the trailing NUL so two 64 KiB libwebsockets fragments cannot write one byte past the heap block (typical READY payloads).
- WebChat inbound WS `rx_buffer_size` is `WS_RX_BUFFER_SIZE` (`WS_TEXT_MAX` plus JSON envelope) so dashboard messages are not split across 256-byte RECEIVE callbacks and dropped.
- WebChat WebSocket sends now accept agent replies up to 32 KiB (`WS_TEXT_MAX`, matching `RESPONSE_BUF_SIZE`) instead of silently dropping payloads above 8 KiB. Dest buffers are `WS_TEXT_BUF_SIZE` so a max-length payload keeps its NUL; a too-large frame is logged instead of skipped with `<`.
- Memory injection is skipped when the system prompt already fills its 64 KiB buffer, instead of writing the full `Relevant memories` prefix past the allocation after clamping recall to 0.
- Session JSON that would exceed the 128 KiB cap is refused instead of truncated, so the next parse cannot wipe history. An oversized stored blob is left in place (distinct `SESSION_LOAD_TOO_LARGE`) rather than replaced by a later small turn.
- Multi-round ReAct copies tool results into the in-flight message list so a later round cannot overwrite earlier outputs.
- `memory_init` no longer deletes an existing SQLite DB when `sqlite3_open` fails (permissions or transient I/O).
- Anthropic `content` parse fails closed when growing the text buffer or `tool_use` array cannot `realloc`, instead of copying against an inflated cap.
- HTTP 200 JSON-RPC results with a malformed ASAP envelope no longer double-free the duplicated request id.
- Inbound ASAP `mcp.tool_call` and `state.query` now hold `agent_lock()` around tool execute and SQLite `g_db` reads, matching `task.request`.
- Inbound `POST /asap` now wires the process provider and tool table into `asap_ctx`, so `task.request` and `mcp.tool_call` dispatch instead of failing with `server missing cfg or provider`.
- `POST /asap` rejects serialized JSON-RPC larger than the 64 KiB gateway HTTP buffer (HTTP 500 / JSON-RPC `-32603`) instead of truncating the body.

### Added
- Discord helper tests reject null MESSAGE_CREATE payloads, bot authors, empty author ids, and guild messages without bot identity so allowlist/mention gating cannot silently widen.
- Phase 5 documentation suite (`docs/SECURITY.md`, `docs/ASAP.md`, and related guides).
- `CONTRIBUTING.md` with PR workflow and pre-tag `gpio-mockup` ritual.
- Jetson-aware `[hardware]` defaults in `config.example.toml` and `.env.example`.

### Changed
- Discord `MESSAGE_CREATE` routing calls `discord_helpers_route_message_create` so helper allowlist/mention tests cover live gating; empty content, strdup, and queue stay in `discord.c`.
- `main` is the active line. On-device Jetson sign-off is a known pending item, not a merge gate ([`docs/JETSON_SIGNOFF.md`](docs/JETSON_SIGNOFF.md)).
- Gateway `/health` `version` matches `SHELLCLAW_RELEASE_VERSION`.

### Security
- Gateway shutdown joins the HTTP thread before `auth_cleanup`, so in-flight `/api/*`, `/pair`, and WebSocket upgrades cannot call `auth_validate_token` / `auth_pair` on a freed `auth_ctx`.
- Gateway listen bind now uses `gateway.host` (`lws` `info.iface`). `host = "127.0.0.1"` is loopback-only. Bind-all forms (`0.0.0.0`, `*`, `::`, `[::]`, empty) require `allow_bind_all`.
- Camera auto-output keeps the exclusive `mkstemp` inode (no unlink + `${tmpl}.jpg` sibling).
- Reject I2C `bus` outside 0–255 at the tool JSON boundary.
- Document that protocol-public `POST /asap` can invoke local tools; production must set `[asap].trusted_senders` before exposing the gateway.

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
