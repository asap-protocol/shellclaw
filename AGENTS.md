# Agent quickstart

ShellClaw is a C99 AI agent for edge hardware (Jetson / Raspberry Pi). Read this before editing code.

## Commands

| Action | Command |
|--------|---------|
| Build | `make shellclaw` → `build/shellclaw` |
| Run all tests | `make test` |
| Static analysis | `make static` (requires cppcheck) |
| ASan + UBSan tests | `make test-sanitize` (requires Clang/GCC sanitizer support) |
| Coverage (core ≥ 80%) | `make coverage` (requires lcov) |
| Full CI locally | `chmod +x scripts/ci-local.sh && ./scripts/ci-local.sh` (Ubuntu 24.04+; on Mac use `CI=true make test`) |
| Jetson on-device HW | `SHELLCLAW_HW_TEST=1 make test_hardware_on_device` (also runs when `SHELLCLAW_HW_TEST=1 make test`) |
| Pre-PR check | `CI=true make clean && CI=true make test` |

Gateway tests need libwebsockets: `GATEWAY=1 make test_gateway_http`.

## Layout

```
src/
  core/       agent loop, config, memory, skills
  providers/  LLM providers (Anthropic, OpenAI, local)
  tools/      shell, file, web_search, asap_invoke, cron, context
  channels/   CLI, Telegram, Discord, WebChat
  gateway/    HTTP server, WebSocket, embedded Web UI
  asap/       ASAP Protocol client/server, registry, manifest
  sandbox/    Linux namespaces + cgroups v2
  hardware/   GPIO/I2C/camera abstraction (per-board backends)
  crypto/     Ed25519 signing
tests/        one test binary per module (Makefile targets)
```

## Conventions

- Language: C99/C11, English comments, Doxygen on public APIs.
- Types: explicit on every variable, parameter, and return value.
- Config/secrets: environment variables and `.env.example` — never commit credentials.
- Tests must run headless with no manual setup or secrets.
- Bug fix: write a failing regression test first, then fix.

## Known debt

- **`tool_X_set_config` setter-global convention (v1.0.1):** the tools (shell, file, web_search, asap_invoke, context, hardware) each expose a module-local mutable config pointer set via a `tool_X_set_config(const config_t *)` setter (e.g. `g_hw_cfg` in `src/tools/hardware_tools_helpers.c`). This avoids passing `const config_t *cfg` through `tool_t.execute` / `agent_tool_t.execute` (an ABI change touching `tool.h`/`agent.h` vtables, 13 tool callbacks, ~30 test sites, >10 files). The whole-convention refactor (pass `const config_t *cfg` or a `tool_context_t` through `execute` for all tools) is scheduled for v1.0.1. Recorded in Phase 5 slice 05 (H1 path B).
- **`src/core/config.c` 1000-line waiver (v1.0.1):** `config.c` is 1261 lines (a single-struct TOML parser where every `parse_*` writes the same `config_t`). The 1000-line rule is a presumptive (rebuttable) blocker. A clean hardware-only extract (~167 lines: `parse_hardware`, `free_hardware_io`, `config_hardware_*` accessors) leaves the file at ~1094 — still over 1k. The proper fix is a two-section extract (`config_hardware.c` + `config_asap.c`, ~331 lines → ~930), scheduled for v1.0.1. The `asap_skill_descriptions` table is ASAP-section code, NOT hardware, and must not move with a hardware extraction. Waived for v1.0.0 per Phase 5 slice 05 (B1).
- **Jetson on-device sign-off (deferred):** Phase 5 software is on `main`. GPIO/I2C/`llama-server` smoke on a physical Jetson Orin Nano Super is **not** a merge gate. Run `SHELLCLAW_HW_TEST=1 make test_hardware_on_device` and [`docs/JETSON_SIGNOFF.md`](docs/JETSON_SIGNOFF.md) when hardware is available.

## Branches

- `main` — active line; open PRs here (or via `development` when integrating a large slice).
- `development` — optional integration branch; no longer a hold-back until Jetson sign-off.
- Phase 5 on-device Jetson validation is a known pending item, not a `development` → `main` blocker. See [`docs/JETSON_SIGNOFF.md`](docs/JETSON_SIGNOFF.md).

## Rules

Project rules live in `.cursor/rules/`:

- `c-principles.mdc` — C coding standards
- `agent-clean-code-c.mdc` — agent-oriented clean code for C
- `shellclaw-architecture.mdc` — product architecture
- `testing.mdc` — test workflow and CI
- `git-commits.mdc` — Conventional Commits
- `karpathy-guidelines.mdc` — behavioral guidelines for agents
