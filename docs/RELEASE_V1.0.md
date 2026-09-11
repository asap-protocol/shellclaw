# v1.0.0 release runbook (operator)

Branch policy: **`development` → `main` is allowed without Jetson sign-off.** On-device validation ([`JETSON_SIGNOFF.md`](JETSON_SIGNOFF.md)) is a known pending item — run it when hardware is available; it does not block merging or continuing work on `main`.

| Phase | Where | Required to merge to `main`? |
|-------|--------|------------------|
| A | x86 / CI gates | Yes |
| B | B1–B6 on device | No (known pending) |
| C | Tag, Pages manifest, marketplace | No (after merge; optional until Jetson rows are filled) |

**Tracking issue:** [`docs/issueops/v1.0.0-jetson-signoff-issue.md`](issueops/v1.0.0-jetson-signoff-issue.md) — `gh issue create --title "v1.0.0 Jetson sign-off" --body-file docs/issueops/v1.0.0-jetson-signoff-issue.md`

**Draft PR body:** [`docs/issueops/pr-development-to-main-v1.0.0.md`](issueops/pr-development-to-main-v1.0.0.md)

---

## Phase A (no Jetson) — verification commands

```bash
CI=true GATEWAY=1 make clean && CI=true GATEWAY=1 make test
./scripts/ci-local.sh   # Ubuntu 24.04+ recommended
make clean && make release && stat -f%z build/shellclaw   # macOS; Linux: stat -c%s
```

Gateway + manifest (agent on `127.0.0.1:18789`):

```bash
GATEWAY=1 make shellclaw
curl -sf http://127.0.0.1:18789/health
curl -sf http://127.0.0.1:18789/.well-known/asap/manifest.json | python3 -m json.tool
./scripts/dump_manifest.sh -o /tmp/manifest.json
./scripts/validate_manifest.sh /tmp/manifest.json
./scripts/run_asap_compliance.sh
GATEWAY=1 ./scripts/bench.sh --json > /tmp/bench-x86.jsonl
```

### Phase A results (2026-06-01, macOS arm64)

| Step | Result |
|------|--------|
| `CI=true GATEWAY=1 make test` | **PASS** (all targets green) |
| `dump_manifest.sh` | **PASS** — SignedManifest with `urn:asap:agent:shellclaw` |
| `validate_manifest.sh` | **SKIP** — `asap` PyPI package not installed (CI-safe skip) |
| `bench.sh --json` | **PASS** — cold start 932 ms, sandbox 3231 µs, HTTP RTT 172 ms, agent loop 1195 ms (see [`BENCHMARKS.md`](BENCHMARKS.md) x86 table) |
| `run_asap_compliance.sh` | **FAIL** (3/3) — health schema missing `agent_id`/`version`/`uptime_seconds`; manifest signature verify failed; `/asap` POST 400 (known v1.0 gaps; see [`ASAP.md`](ASAP.md)) |
| `gpio-mockup` | **BLOCKED** on macOS — Linux-only kernel module |
| `test_hardware_on_device` (no gate) | **PASS** — exits 0 skip |

**gpio-mockup** (Linux laptop, before tag): [`CONTRIBUTING.md`](../CONTRIBUTING.md) § Pre-tag release ritual.

```bash
sudo modprobe gpio-mockup gpio_mockup_ranges=-1,32
SHELLCLAW_BOARD=jetson make test_hardware_libgpiod
sudo rmmod gpio-mockup
make static
make test-sanitize
```

---

## Phase B — Jetson sign-off (B1–B6)

Full checklist: [`JETSON_SIGNOFF.md`](JETSON_SIGNOFF.md). Summary:

### B1 — Setup

```bash
nvpmodel -q
gpiodetect
```

### B2 — Install + services

```bash
git checkout development && git pull
make shellclaw
./scripts/install.sh
./scripts/build_llama_jetson.sh
./scripts/download_model.sh phi3
systemctl --user enable --now llama-server shellclaw
curl -sf http://localhost:18789/health
curl -sf http://127.0.0.1:8080/v1/models
```

### B3 — Functional (manual)

- Web/Telegram: `gpio_read(13)`, `i2c_scan(7)` (empty OK), local provider when cloud off.

### B4 — On-device runner

```bash
export SHELLCLAW_HW_TEST=1
make test_hardware_on_device
```

Uses [`tests/test_hardware_on_device.sh`](../tests/test_hardware_on_device.sh): board detect, GPIO on `gpio_test_pin`, I2C scan, `llama-server` HTTP smoke. **Without `SHELLCLAW_HW_TEST=1` the script exits 0 (skip).**

### B5 — Quality + benchmarks

```bash
make static
make test-sanitize
make release && stat -c%s build/shellclaw

sudo nvpmodel -m 0 && sudo jetson_clocks
BENCH_SET_POWER_MODE=1 ./scripts/bench.sh --power-mode MAXN_SUPER --storage nvme
sudo nvpmodel -m 1
BENCH_SET_POWER_MODE=1 ./scripts/bench.sh --power-mode 15W --storage nvme
```

Paste results into [`BENCHMARKS.md`](BENCHMARKS.md).

### B6 — Pre-tag manifest (local)

```bash
GATEWAY=1 make shellclaw
./scripts/dump_manifest.sh -o /tmp/manifest.json
python3 -m asap.crypto.verify_manifest /tmp/manifest.json   # pip install asap
```

---

## Phase C — merge, tag, marketplace

### C1 — Draft PR `development` → `main`

```bash
gh pr create --base main --head development \
  --title "release: v1.0.0 edge Jetson foundation" \
  --body-file docs/issueops/pr-development-to-main-v1.0.0.md \
  --draft
```

Jetson sign-off is optional for this PR. Record it as known pending unless the on-device checklist is already done.

### C2 — After merge to `main`

```bash
git checkout main && git pull
git tag -a v1.0.0 -m "ShellClaw v1.0.0 — Jetson edge agent"
git push origin v1.0.0
```

### C3 — Post-tag automation

1. **GitHub Actions** — [`.github/workflows/publish-manifest.yml`](../.github/workflows/publish-manifest.yml) runs on `v*` tag push; deploys `docs/manifest.json` to GitHub Pages.
2. **Verify Pages manifest:**

```bash
curl -fsS https://asap-protocol.github.io/shellclaw/manifest.json -o /tmp/pages-manifest.json
python3 -m asap.crypto.verify_manifest /tmp/pages-manifest.json
```

3. **ASAP marketplace IssueOps** (only after C3 step 2 passes):
   - Prefill: [`docs/issueops/register-agent-prefill.md`](issueops/register-agent-prefill.md)
   - Or: `./scripts/open_marketplace_registration.sh`
   - Direct: https://github.com/asap-protocol/asap-protocol/issues/new?template=register_agent.yml
   - Record issue URL in [`MARKETPLACE_STATUS.md`](MARKETPLACE_STATUS.md)
4. **Marketplace verify** — [`docs/issueops/VERIFY_MARKETPLACE.md`](issueops/VERIFY_MARKETPLACE.md); set `MARKETPLACE_STATUS.md` to **listed** when Browse UI passes.

### C4 — Changelog and index

- Set `CHANGELOG.md` `[1.0.0]` date (currently **TBD**).
- Confirm [`00-index.md`](../.cursor/dev-planning/tasks/phase5/00-index.md) §105–122 sign-off items (do not edit plan unless maintainer approves).

---

## Plan checklist corrections (read-only)

If using plan file §189–226 directly, prefer [`JETSON_SIGNOFF.md`](JETSON_SIGNOFF.md) for these fixes:

| Plan line | Use instead |
|-----------|-------------|
| `make test_hardware_on_device` | `SHELLCLAW_HW_TEST=1 make test_hardware_on_device` |
| `make test` + ASan | `make test-sanitize` |
| `download_model.sh` (no arg) | `./scripts/download_model.sh phi3` |
| `python -c "import nacl.signing; ..."` | `python3 -m asap.crypto.verify_manifest` |
