# Local inference (llama.cpp)

ShellClaw uses an external **`llama-server`** process (from [llama.cpp](https://github.com/ggml-org/llama.cpp)) as an OpenAI-compatible chat backend. The agent binary talks HTTP only — it does not link CUDA or GGML. Configuration: `[providers.local]` in `~/.shellclaw/config.toml` and `/etc/shellclaw/llama-server.env` for the systemd unit.

For software architecture, see [ARCHITECTURE.md](ARCHITECTURE.md). For Jetson setup order, see [HARDWARE_JETSON.md](HARDWARE_JETSON.md).

---

## Architecture

```
ShellClaw (providers/local.c)
    │  POST /v1/chat/completions
    ▼
llama-server (127.0.0.1:8080)
    │  CUDA on Jetson / CPU on RPi
    ▼
GGUF model on disk (/var/lib/shellclaw/models/)
```

At startup, `local.c` probes `GET /health` and `GET /v1/models`. If unreachable, the local provider marks itself unavailable and the router falls through to the next entry in `providers.fallback_chain`.

---

## Build scripts and CMake flags

Pinned upstream tag: **`b9087`** (2026-05-09) on both boards.

### Jetson Orin Nano Super (CUDA)

```bash
./scripts/build_llama_jetson.sh
```

| CMake flag | Value | Purpose |
|------------|-------|---------|
| `GGML_CUDA` | `ON` | CUDA backend |
| `CMAKE_CUDA_ARCHITECTURES` | `87` | Ampere sm_87 (Orin) |
| `CMAKE_CUDA_COMPILER` | `/usr/local/cuda/bin/nvcc` | Override with `CMAKE_CUDA_COMPILER` env |
| `CMAKE_BUILD_TYPE` | `Release` | Production build |

Verify:

```bash
llama-server --version   # must mention CUDA / cuBLAS / GPU
```

Expect ~8 minutes on Orin Nano Super with active cooling (`nproc` parallel jobs).

Environment overrides: `LLAMA_SRC_DIR`, `INSTALL_BIN_DIR`, `FORCE_REBUILD=1`, `SKIP_ARCH_CHECK=1` (lint only).

### Raspberry Pi Zero 2 W (CPU — Phase 6 validation)

```bash
./scripts/build_llama_rpi.sh
```

| CMake flag | Value | Purpose |
|------------|-------|---------|
| `GGML_CUDA` | `OFF` | No GPU |
| `GGML_NATIVE` | `ON` | `-march=native` tuning on device |

Verify: `llama-server --version` must **not** report CUDA/GPU.

---

## systemd integration

`./scripts/install.sh` copies board-specific env to `/etc/shellclaw/llama-server.env` and installs user units:

```bash
systemctl --user enable --now llama-server.service shellclaw.service
```

`llama-server.service` exec line:

```
/usr/local/bin/llama-server --model $MODEL --host 127.0.0.1 --port $PORT -t $THREADS -ngl $NGL -c $CTX_SIZE
```

### Jetson defaults (`systemd/llama-server.jetson.env`)

| Variable | Default | Meaning |
|----------|---------|---------|
| `MODEL` | `Phi-3-mini-4k-instruct-Q4_K_M.gguf` | Q4_K_M quant |
| `THREADS` | `6` | CPU threads for non-GPU ops |
| `NGL` | `999` | Offload all layers to GPU |
| `PORT` | `8080` | Must match `[providers.local] endpoint` |
| `CTX_SIZE` | `4096` | Context window tokens |

### RPi defaults (`systemd/llama-server.rpi.env`)

| Variable | Default |
|----------|---------|
| `MODEL` | `tinyllama-1.1b-chat-Q4_K_M.gguf` |
| `THREADS` | `4` |
| `NGL` | `0` (CPU only) |
| `CTX_SIZE` | `2048` |

Restart after model swap:

```bash
systemctl --user restart llama-server.service
```

---

## Download models

```bash
./scripts/download_model.sh phi3        # Jetson default
./scripts/download_model.sh tinyllama   # RPi default
```

Destination: `/var/lib/shellclaw/models/` (override with `MODEL_DIR`).

Optional supply-chain check: set `EXPECTED_SHA256` (or `PHI3_SHA256` / `TINYLLAMA_SHA256`) from the Hugging Face file metadata page. This repo does not vendor a default digest.

---

## Recommended models

| Board | Default (Q-MODEL) | Size (approx) | Expected throughput |
|-------|-------------------|---------------|---------------------|
| Jetson Orin Nano Super | **Phi-3-mini-4k-instruct Q4_K_M** | ~2.3 GB | 25–35 tok/s MAXN_SUPER (target ≥20) |
| Jetson (optional) | Llama-3.1-8B Q4 | ~4.7 GB | 14–18 tok/s — tighter memory budget |
| RPi Zero 2 W | **TinyLlama 1.1B Q4_K_M** | ~0.7 GB | Emergency fallback only; cloud primary |

Manifest advertises Jetson local model id `Phi-3-mini-4k-instruct-Q4_K_M` (`src/asap/manifest.c`).

**v1.0 gate:** Phi-3-mini Q4 ≥ 20 tok/s on MAXN_SUPER (or documented alternative per Q-MODEL). Publish numbers in [BENCHMARKS.md](BENCHMARKS.md) when available.

---

## Unified memory budgeting (Jetson)

Jetson Orin Nano Super **8 GB** uses **unified memory** — CPU and GPU share the same physical pool. There is no separate VRAM bar.

### Planning budget (Phi-3-mini Q4, ctx 4096)

| Consumer | Approx RAM |
|----------|------------|
| L4T + desktop/services | 1.5–2.5 GB |
| `llama-server` + weights + KV cache | 2.5–3.5 GB |
| ShellClaw agent | < 15 MB active |
| Headroom for spikes | ≥ 1 GB |

**Rules of thumb:**

- If `tegrastats` shows RAM **> 7 GB** used under load, reduce `CTX_SIZE`, use a smaller quant, or stop other services.
- **Llama-3.1-8B Q4** fits but leaves little margin — close browsers, avoid parallel heavy jobs.
- Account **`llama-server` separately** from agent RAM in benchmarks (PRD §4.10).

### Monitoring — use tegrastats, not nvidia-smi

**`nvidia-smi` is NOT available on Tegra** (Jetson). It is for discrete/datacenter GPUs. On Jetson use:

```bash
tegrastats --interval 1000
```

Example fields (JetPack 6.2.x):

- `RAM used/total MB` — unified memory pressure
- `GR3D_FREQ X%@[Y,Z]` — GPU utilization and frequency
- `gpu@XX.XC` — GPU temperature

ShellClaw parses one-shot samples for `/api/hardware/gpu` (`src/hardware/hardware_tegrastats.c`). Regex pinned to JetPack 6.2.x output — re-validate after JetPack upgrades.

Power mode affects throughput and thermals:

```bash
nvpmodel -q
```

See [HARDWARE_JETSON.md](HARDWARE_JETSON.md) — Super 8 GB has MAXN_SUPER and 15W; no 7W mode.

---

## Provider configuration

`config.example.toml`:

```toml
[providers.local]
endpoint = "http://127.0.0.1:8080/v1/chat/completions"
model = "tinyllama-1.1b-q4"   # logical name; must match llama-server loaded GGUF alias
```

Fallback chain example (Jetson edge briefing):

```toml
[providers]
fallback_chain = ["anthropic", "local", "stub"]
```

When cloud is unreachable, router selects `local` if probe succeeds. Check active backend: `GET /api/status` (Bearer auth).

---

## Ollama caveats

ShellClaw **does not ship or require Ollama**. Supported path is **`llama-server`** built from pinned llama.cpp via project scripts.

If you point `[providers.local] endpoint` at Ollama's OpenAI-compatible URL (`http://127.0.0.1:11434/v1/chat/completions`):

| Topic | Caveat |
|-------|--------|
| Support | **Best-effort only** — not tested in CI or release checklist |
| Memory | Ollama daemon adds overhead vs bare `llama-server` on 8 GB unified memory |
| CUDA | Ollama Jetson builds vary by community recipe; may not match sm_87 flags in `build_llama_jetson.sh` |
| Model paths | Ollama manages its own model store — manifest `local_model_id` may not match |
| systemd | Use Ollama's unit, not `llama-server.service`, and update endpoint accordingly |

For reproducible v1.0 sign-off, use **`./scripts/build_llama_jetson.sh`** + **`./scripts/download_model.sh phi3`**.

---

## Smoke test

```bash
curl -sf http://127.0.0.1:8080/health
curl -sf http://127.0.0.1:8080/v1/models

curl -sf http://127.0.0.1:8080/v1/chat/completions \
  -H 'Content-Type: application/json' \
  -d '{"model":"local","messages":[{"role":"user","content":"Say OK"}],"max_tokens":16}'
```

On-device gate (v1.0): `SHELLCLAW_HW_TEST=1 make test_hardware_on_device` includes local inference smoke against `llama-server`.

---

## Troubleshooting

| Symptom | Action |
|---------|--------|
| Local provider skipped | `systemctl --user status llama-server`; verify `/health` |
| OOM / killed llama-server | Reduce model size or `CTX_SIZE`; check `tegrastats` RAM |
| Slow first token | NVMe vs microSD; cold GPU clock — wait for warmup |
| CUDA not used | Rebuild with `build_llama_jetson.sh`; confirm `-ngl 999` in env |
| Wrong model name | Align `model` in config with GGUF filename / server alias |

---

## Related docs

- [HARDWARE_JETSON.md](HARDWARE_JETSON.md) — power modes, cooler, NVMe
- [BENCHMARKS.md](BENCHMARKS.md) — tok/s and RAM metrics (when published)
- [ARCHITECTURE.md](ARCHITECTURE.md) — provider router
