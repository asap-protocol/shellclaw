# Jetson v1.0.0 sign-off (operator checklist)

**Status: known pending — not a merge gate.** Phase 5 software lives on `main`. Run this checklist when a Jetson Orin Nano Super is available; do not block PRs or `development` → `main` on it.

Copy-paste ritual for **Jetson Orin Nano Super** before tagging `v1.0.0` with on-device confidence. Mirrors [`.cursor/dev-planning/tasks/phase5/04-release-quality.md`](../.cursor/dev-planning/tasks/phase5/04-release-quality.md) § Manual on-device validation, with **corrected commands** where the plan omits env vars.

**Out of scope for v1.0:** BME280/BH1750 reads, CSI/USB camera E2E, `home-monitor` / `visual-monitor` skills (v1.2).

Tracking: keep this checklist in-repo. GitHub issues may be disabled; use [`docs/issueops/v1.0.0-jetson-signoff-issue.md`](issueops/v1.0.0-jetson-signoff-issue.md) if you open a tracking issue later.

---

## B1 — Hardware setup

- [ ] Fresh JetPack **6.2.x** boots (microSD or NVMe; NVMe strongly recommended for benchmarks)
- [ ] Active cooler mounted and audible under load
- [ ] Power modes available:

```bash
nvpmodel -q
nvpmodel -q --verbose   # note mode ids for MAXN_SUPER and 15W
```

- [ ] GPIO chips present:

```bash
gpiodetect
# Expect gpiochip0 (tegra234-gpio) and gpiochip1 (tegra234-gpio-aon)
```

---

## B2 — Build, install, services

From repo root on the Jetson:

```bash
git checkout main && git pull
make shellclaw
./scripts/install.sh
./scripts/build_llama_jetson.sh
./scripts/download_model.sh phi3
systemctl --user daemon-reload
systemctl --user enable --now llama-server.service shellclaw.service
```

- [ ] `./scripts/install.sh` completes; user units enabled
- [ ] `./scripts/build_llama_jetson.sh` completes (`llama-server` on PATH)
- [ ] `./scripts/download_model.sh phi3` fetches Phi-3-mini Q4_K_M
- [ ] `systemctl --user start llama-server shellclaw` succeeds
- [ ] Health:

```bash
curl -sf http://localhost:18789/health
curl -sf http://127.0.0.1:8080/v1/models | head -c 200
```

- [ ] Pair via Web UI; bearer token works
- [ ] `/hardware` — Board + GPIO + GPU populated; Sensors + Camera tabs show **Coming in v1.2**

---

## B3 — Functional (LLM end-to-end)

With agent running and local inference up:

- [ ] LLM call exercises `gpio_read(13)` via Telegram or web chat
- [ ] LLM call exercises `i2c_scan(7)` (empty array OK — no sensors wired)
- [ ] Cloud disabled → `/api/status` shows **local** provider active

---

## B4 — On-device automated runner

**Required:** `SHELLCLAW_HW_TEST=1` (without it the script exits 0 and does not test hardware).

```bash
export SHELLCLAW_HW_TEST=1
# Optional overrides: SHELLCLAW_GPIO_TEST_PIN SHELLCLAW_I2C_BUS SHELLCLAW_LLAMA_URL SHELLCLAW_CONFIG
make test_hardware_on_device
```

- [ ] Exit code **0** and lines for GPIO, I2C bus scan, llama-server smoke

Wrong board with `SHELLCLAW_HW_TEST=1` exits **77** — fix `SHELLCLAW_BOARD` / device tree before sign-off.

---

## B5 — Quality gates and benchmarks

**Laptop / CI ritual (before tag, any machine with libgpiod):**

```bash
sudo modprobe gpio-mockup gpio_mockup_ranges=-1,32
ls /dev/gpiochip*
SHELLCLAW_BOARD=jetson make test_hardware_libgpiod
sudo rmmod gpio-mockup
```

**On Jetson or x86 dev machine:**

```bash
make static                    # cppcheck — zero findings
make test-sanitize             # ASan + UBSan (not plain make test)
make release && stat -c%s build/shellclaw   # Linux; expect < 600 KB
```

**Fill [`BENCHMARKS.md`](BENCHMARKS.md) NVMe rows (MAXN_SUPER + 15W):**

```bash
sudo nvpmodel -m 0 && sudo jetson_clocks
BENCH_SET_POWER_MODE=1 ./scripts/bench.sh --power-mode MAXN_SUPER --storage nvme
sudo nvpmodel -m 1
BENCH_SET_POWER_MODE=1 ./scripts/bench.sh --power-mode 15W --storage nvme
```

- [ ] `gpio-mockup` ritual passed
- [ ] `make static` zero findings
- [ ] `make test-sanitize` green
- [ ] Release binary under 600 KB
- [ ] `docs/BENCHMARKS.md` has MAXN_SUPER + 15W Jetson numbers (NVMe column)

---

## B6 — Release artifacts (pre-merge / pre-tag)

On a machine with gateway build and keys under `~/.shellclaw/keys/` (0600):

```bash
GATEWAY=1 make shellclaw
./scripts/dump_manifest.sh -o /tmp/manifest.json
./scripts/validate_manifest.sh /tmp/manifest.json
pip install 'asap>=0.1'   # once per env
python3 -m asap.crypto.verify_manifest /tmp/manifest.json
```

After **`v1.0.0` tag** (Phase C — not before):

```bash
curl -fsS https://asap-protocol.github.io/shellclaw/manifest.json | python3 -m asap.crypto.verify_manifest
```

- [ ] Signed manifest verifies locally before tag
- [ ] GitHub Pages manifest live and verifies **after** tag ([`publish-manifest.yml`](../.github/workflows/publish-manifest.yml))
- [ ] ASAP marketplace IssueOps filed **after** Pages URL live — [`register-agent-prefill.md`](issueops/register-agent-prefill.md)
- [ ] [`CHANGELOG.md`](../CHANGELOG.md) v1.0.0 entry finalized (date, accurate security notes)
- [ ] README roadmap includes Phase 7 (v1.2) row

---

## Sign-off record

| Field | Value |
|-------|--------|
| Jetson hostname / JetPack | |
| Storage (NVMe / microSD) | |
| Operator | |
| Date | |
| Git commit on device | `git rev-parse --short HEAD` |
| Issue URL | |

When B1–B6 are complete, comment on the tracking issue and proceed to [`RELEASE_V1.0.md`](RELEASE_V1.0.md) Phase C.
