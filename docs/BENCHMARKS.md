# ShellClaw performance benchmarks

PRD §4.10 / Phase 5 Wave 8. Numbers below compare **Jetson Orin Nano Super 8 GB** (MAXN_SUPER + 15 W), **x86 baseline**, and **boot storage** (NVMe vs microSD per Q-NVMe).

Run the harness on device:

```bash
make shellclaw
chmod +x scripts/bench.sh

# MAXN_SUPER (25 W TDP) — verify mode ids with: nvpmodel -q --verbose
sudo nvpmodel -m 0 && sudo jetson_clocks
BENCH_SET_POWER_MODE=1 ./scripts/bench.sh --power-mode MAXN_SUPER --storage nvme

# 15 W sustainable mode
sudo nvpmodel -m 1
BENCH_SET_POWER_MODE=1 ./scripts/bench.sh --power-mode 15W --storage nvme
```

On a developer laptop (no Jetson), `./scripts/bench.sh` still runs: sandbox, cold-start, RAM, gateway RTT, and stub agent-loop sections emit live numbers; Jetson-only rows stay `skip` until you re-run on hardware.

**Power-mode gotcha:** Jetson Orin Nano **Super 8 GB** firmware removed the legacy **7 W** mode. Only MAXN_SUPER (~25 W) and **15 W** are available for v1.0 sign-off.

**Model default (Q-MODEL):** Phi-3-mini-4k-instruct Q4_K_M via `llama-server` (CUDA). Llama-3.1-8B Q4 optional second row when disk and thermals allow.

---

## How to read these tables

| Column | Meaning |
|--------|---------|
| **Target** | PRD acceptance threshold where defined |
| **NVMe** | Boot/root on NVMe SSD (recommended, Q-NVMe) |
| **microSD** | Boot/root on microSD (valid but I/O-bound for cold start) |
| **x86** | Linux/macOS dev machine baseline (no CUDA unless noted) |

RAM figures are **ShellClaw agent process only** (`VmRSS`); `llama-server` unified memory is reported separately via `tegrastats` during LLM benchmarks.

---

## Summary (fill on Jetson — placeholders until device run)

### Jetson Orin Nano Super 8 GB — MAXN_SUPER

| Metric | Target | NVMe | microSD | Notes |
|--------|--------|------|---------|-------|
| Cold start (gateway `/health`) | < 1 s | _run on device_ | _run on device_ | `./scripts/bench.sh --section cold_start` |
| Idle RAM (agent) | < 5 MB | _run on device_ | _run on device_ | Excludes `llama-server` |
| Active RAM (agent) | < 15 MB | _run on device_ | _run on device_ | After `/api/status`; full LLM path higher |
| Sandbox `clone()` median | < 1 ms | _run on device_ | _run on device_ | Wraps `build/test_sandbox` |
| I2C scan bus 7 | — | _run on device_ | _run on device_ | `i2cdetect -y 7`; empty bus OK |
| Camera capture cold | — | _run on device_ | _run on device_ | CSI + `nvarguscamerasrc`; needs camera |
| Camera capture warm | — | _run on device_ | _run on device_ | Second shot after pipeline warm |
| Gateway HTTP RTT median | — | _run on device_ | _run on device_ | `/health` proxy; 10 samples |
| Phi-3-mini gen tok/s | ≥ 20 | _run on device_ | — | NVMe strongly recommended for LLM I/O |
| Phi-3-mini prefill tok/s | — | _run on device_ | — | ~2k-token prompt, `max_tokens=1` |
| Unified RAM at LLM (tegrastats) | — | _run on device_ | — | GPU + CPU unified on Tegra |
| Agent loop (stub `-m`) | — | _run on device_ | _run on device_ | Re-run with `fallback_chain = ["local"]` for LLM e2e |

### Jetson Orin Nano Super 8 GB — 15 W

Same metrics as MAXN_SUPER; expect lower LLM tok/s and GPU clocks. Re-run `./scripts/bench.sh` after `sudo nvpmodel -m 1`.

| Metric | Target | NVMe | microSD |
|--------|--------|------|---------|
| Phi-3-mini gen tok/s | ≥ 20 (PRD MAXN) | _run on device_ | — |
| Cold start | < 1 s | _run on device_ | _run on device_ |
| Sandbox median | < 1 ms | _run on device_ | _run on device_ |

### x86 baseline (comparison)

Captured on a developer workstation for relative overhead (not a release gate).

**Operator step:** fill the x86 column by running `GATEWAY=1 make shellclaw && ./scripts/bench.sh --json > bench-x86.jsonl` on your laptop, then copy non-`skip` metrics into the table below. Jetson NVMe/microSD columns stay `_run on device_` until [on-device sign-off](JETSON_SIGNOFF.md) (known pending).

| Metric | Example / placeholder | How |
|--------|----------------------|-----|
| Cold start | _run locally_ | `./scripts/bench.sh --section cold_start` |
| Idle RAM | _run locally_ | Ephemeral gateway |
| Sandbox median | _run locally_ | Linux namespaces; macOS runs bench without namespace isolation tests |
| Gateway HTTP RTT | _run locally_ | Ephemeral gateway on loopback |
| Phi-3-mini tok/s | N/A (no Tegra GPU) | Skip unless local CUDA llama-server |
| tegrastats | skip | Jetson only |

---

## Harness reference

### `scripts/bench.sh`

Wraps:

- **`build/test_sandbox`** — sandbox `clone()` / `sandbox_exec("true")` median (PRD §4.7.35 / test 5.7).
- **`tegrastats --interval 100 --count 1`** — one-shot RAM, GR3D, GPU temp (same command as `hardware_tegrastats.c`).
- **`nvpmodel -q`** — power mode label for result rows.
- **Ephemeral gateway boot** — pattern from `scripts/dump_manifest.sh` for cold start, RAM, HTTP RTT.
- **`i2cdetect`**, **`gst-launch-1.0` + `nvarguscamerasrc`** — on-device I2C / camera latency when hardware present.
- **`curl` → `llama-server` `/v1/chat/completions`** — generation and prefill tok/s when server is up.

Sections: `meta`, `tegrastats`, `cold_start`, `ram`, `sandbox`, `i2c`, `camera`, `websocket`, `llm`, `agent_loop`.

```bash
./scripts/bench.sh --help
./scripts/bench.sh --json                    # machine-readable
./scripts/bench.sh --section sandbox         # single metric family
make bench                                   # build + run full harness
```

Environment variables are documented in the script header (`BENCH_LLAMA_URL`, `BENCH_GATEWAY_URL`, `BENCH_I2C_BUS`, …).

**Dependencies:** bash, `curl`, and standard build tools. Timestamps use `date +%s%3N` on Linux when available; macOS may use `gdate` or optional `python3` for millisecond precision.

### Makefile

```bash
make bench    # chmod +x scripts/bench.sh && ./scripts/bench.sh
```

---

## Recording results (release ritual)

1. Flash JetPack **6.2.x**, mount **active cooler**, confirm `gpiodetect` shows `tegra234-gpio` chips.
2. Prefer **NVMe root** before Wave 8 numbers ([`jetsonhacks/migrate-jetson-to-ssd`](https://github.com/jetsonhacks/migrate-jetson-to-ssd)).
3. `./scripts/install.sh`, `./scripts/build_llama_jetson.sh`, `./scripts/download_model.sh phi3`.
4. `systemctl --user start llama-server shellclaw` — confirm `curl -sf http://127.0.0.1:8080/v1/models`.
5. Run bench in **both** power modes; paste `key=value` output into the tables above (separate NVMe and microSD columns if you have both setups).
6. Optional: `Llama-3.1-8B Q4_K_M` row with `BENCH_LLM_MODEL=...` when model fits in 8 GB unified memory at 2k context.

**Sign-off gate (Phase 5 checklist):** `docs/BENCHMARKS.md` published with at least **MAXN_SUPER + 15 W** Jetson rows filled on real hardware.

---

## Storage note (Q-NVMe)

microSD is acceptable for development but **contaminates cold-start and active-RAM measurements** with SD I/O latency. Publish **both** NVMe and microSD rows where the metric is storage-sensitive (cold start, LLM load). LLM tok/s is primarily GPU-bound once the model is resident — still prefer NVMe for honest v1.0 numbers.

---

## Related docs

- [`LOCAL_INFERENCE.md`](LOCAL_INFERENCE.md) — build flags, unified memory, `tegrastats` (no `nvidia-smi` on Tegra).
- [`HARDWARE_JETSON.md`](HARDWARE_JETSON.md) — flash, NVMe migration, power modes, pin map.
- PRD §4.10 — full metric list and acceptance targets.
