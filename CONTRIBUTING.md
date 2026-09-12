# Contributing to ShellClaw

Thank you for helping improve ShellClaw. This guide covers workflow, standards, and release rituals. Deep dives live in [`docs/`](docs/) and [`.cursor/rules/`](.cursor/rules/) — link there instead of copying long sections into PRs.

## Quick start

1. Read [`AGENTS.md`](AGENTS.md) for build commands, module layout, and branch policy.
2. Copy [`config.example.toml`](config.example.toml) to `~/.shellclaw/config.toml` and [`.env.example`](.env.example) to `.env` (never commit `.env`).
3. Build and test:
   ```bash
   make shellclaw
   CI=true make clean && CI=true make test
   ```
   Gateway tests need libwebsockets: `GATEWAY=1 make test_gateway_http`.

## Branches and pull requests

| Branch | Purpose |
|--------|---------|
| `main` | Active line — open PRs here by default |
| `development` | Optional integration branch for large slices |

On-device Jetson validation is a **known pending** item ([`docs/JETSON_SIGNOFF.md`](docs/JETSON_SIGNOFF.md)); it does not block merging to `main`.

**PR checklist**

- [ ] One functional change per PR when possible; split diffs over ~10 files / ~300 lines.
- [ ] [Conventional Commits](https://www.conventionalcommits.org/) messages (`feat:`, `fix:`, `docs:`, `test:`, etc.).
- [ ] `CI=true make clean && CI=true make test` passes (matches Linux CI with `-Werror`).
- [ ] Bug fixes include a failing regression test first, then the fix.
- [ ] No secrets, API keys, or real tokens in code or commits.

**Recommended before merge (release-quality changes)**

```bash
make static              # cppcheck — zero findings expected at release
make test-sanitize       # AddressSanitizer + UBSan full suite
./scripts/ci-local.sh    # full CI mirror on Ubuntu 24.04+
```

See [`.cursor/rules/git-commits.mdc`](.cursor/rules/git-commits.mdc) and [`.cursor/rules/testing.mdc`](.cursor/rules/testing.mdc) for commit format and test workflow details.

## Coding standards

ShellClaw is C99/C11. Follow the project rule files (summarized here — full detail in-repo):

| Topic | Rule file |
|-------|-----------|
| Types, memory, errors, security | [`.cursor/rules/c-principles.mdc`](.cursor/rules/c-principles.mdc) |
| Grep-friendly names, file size, comments | [`.cursor/rules/agent-clean-code-c.mdc`](.cursor/rules/agent-clean-code-c.mdc) |
| Module layout and constraints | [`.cursor/rules/shellclaw-architecture.mdc`](.cursor/rules/shellclaw-architecture.mdc) |

**Essentials**

- Explicit types on every variable, parameter, and return value.
- Public APIs documented with Doxygen in headers; one `.c` + one `.h` per module.
- New tools → `src/tools/<name>.c` + `tests/test_<name>.c` + Makefile target.
- New hardware backend → `src/hardware/` with board-specific code under `src/hardware/boards/`.
- Config and secrets via TOML + environment variables only — see [`.env.example`](.env.example).
- Thread safety: inbound HTTP/WebSocket paths must use `agent_lock()` / `agent_unlock()` around `agent_run()`, inbound ASAP `mcp.tool_call` execute, and `state.query` memory-store reads (see README § Thread Safety).

## Testing

| Goal | Command |
|------|---------|
| All unit tests | `make test` |
| Single module | `make test_<module>` |
| Static analysis | `make static` |
| Sanitizers | `make test-sanitize` |
| Coverage (core ≥ 80%) | `make coverage` |
| Performance harness | `make bench` / [`scripts/bench.sh`](scripts/bench.sh) (bash + curl; optional `python3` on macOS for sub-ms timestamps) |

Tests must run headless with no manual setup or secrets. Hardware-specific on-device tests use `SHELLCLAW_HW_TEST=1` (Jetson only; see slice 04 task 12.x in dev planning):

```bash
export SHELLCLAW_HW_TEST=1
make test_hardware_on_device   # skip on laptop; runs GPIO + I2C + llama-server smoke on Jetson
```

## Documentation

When your change affects behavior, update the relevant doc (or add a link from README) instead of duplicating prose:

| Doc | Scope |
|-----|--------|
| [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) | As-built module map and data flow |
| [`docs/HARDWARE_JETSON.md`](docs/HARDWARE_JETSON.md) | JetPack flash, NVMe, pin map, wiring |
| [`docs/HARDWARE_SAFETY.md`](docs/HARDWARE_SAFETY.md) | 3V3 logic, current limits, ESD |
| [`docs/SECURITY.md`](docs/SECURITY.md) | Threat model, sandbox audit, gateway auth |
| [`docs/ASAP.md`](docs/ASAP.md) | Manifest, marketplace registration, compliance |
| [`docs/LOCAL_INFERENCE.md`](docs/LOCAL_INFERENCE.md) | llama.cpp build, models, Jetson memory |
| [`docs/BENCHMARKS.md`](docs/BENCHMARKS.md) | Performance numbers per board and power mode |
| [`docs/JETSON_SIGNOFF.md`](docs/JETSON_SIGNOFF.md) | On-device Jetson checklist (known pending, not a merge gate) |

## Pre-tag release ritual (maintainers)

CI compile-only smoke does not exercise real GPIO file descriptors. Before tagging a release (e.g. `v1.0.0`), run the `gpio-mockup` ritual below. Jetson on-device sign-off is **known pending** — skip until hardware is available; it does not block `main`.

### 1. `gpio-mockup` local validation (Linux only; requires libgpiod)

**Platform:** `gpio-mockup` is a Linux kernel module — not available on macOS or Windows. Run on Ubuntu 24.04+ (or any Linux host with `libgpiod` 2.x and kernel mockup support) before tagging a release.

Confirms the libgpiod backend opens and exercises a real device fd at least once per release:

```bash
sudo modprobe gpio-mockup gpio_mockup_ranges=-1,32
ls /dev/gpiochip*   # expect a new mockup chip
SHELLCLAW_BOARD=jetson make test_hardware_libgpiod
sudo rmmod gpio-mockup
```

**Verify:** `test_hardware_libgpiod` reports successful read/write/mode against the mockup chip (or skips I/O cleanly when mockup is absent).

### 2. Jetson on-device sign-off (known pending — not a merge gate)

On a wired Jetson Orin Nano Super (with `llama-server` on port 8080). Skip until hardware is available; software continues on `main`.

```bash
export SHELLCLAW_HW_TEST=1
make test_hardware_on_device   # GPIO (gpio_test_pin), I2C scan, llama HTTP smoke
```

Without `SHELLCLAW_HW_TEST=1`, the runner exits 0 immediately (CI-safe). Also complete the manual checklist in [`.cursor/dev-planning/tasks/phase5/04-release-quality.md`](.cursor/dev-planning/tasks/phase5/04-release-quality.md) (install, health, quality gates).

**Do not gate v1.0 on:** BME280 / BH1750 sensor reads, CSI/USB camera capture end-to-end, or `home-monitor` / `visual-monitor` skills — those ship in **v1.2 (Phase 7)**.

## Security

Report suspected vulnerabilities privately to the maintainers (do not open public issues for exploit details). See [`docs/SECURITY.md`](docs/SECURITY.md) for the v1.0 self-audit scope and known limitations.

## License

By contributing, you agree that your contributions are licensed under the [MIT License](LICENSE).
