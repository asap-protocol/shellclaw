# ShellClaw and ASAP Protocol

ShellClaw participates in the [ASAP Protocol](https://github.com/asap-protocol/asap-protocol) ecosystem with an Ed25519-signed manifest (v2.4 schema), structured `capabilities.hardware` and `capabilities.inference` fields, and a static public manifest URL for marketplace discovery.

**Production:** Set `[asap] public_base_url` in `~/.shellclaw/config.toml` to your real HTTPS origin before registry submission. The default `https://shellclaw.example.com` in `config.example.toml` is for local development only; `endpoints.asap` in the signed manifest is built from `public_base_url`.

See also: [ARCHITECTURE.md](ARCHITECTURE.md) (ASAP module layout), [SECURITY.md](SECURITY.md) (Ed25519 key permissions).

---

## Manifest endpoint

When the gateway is enabled and signing keys load successfully, ShellClaw serves ASAP discovery routes:

| Route | Auth | Response |
|-------|------|----------|
| `GET /.well-known/asap/manifest.json` | Public | **SignedManifest** JSON (inner manifest + Ed25519 signature + public_key) |
| `GET /.well-known/asap/health` | Public | Minimal health stub (`{"status":"ok"}` in v1.0) |
| `POST /asap` | Rate-limited | JSON-RPC ASAP task ingress (partial v1.0) |
| `GET /api/asap/log` | Bearer | Inbound ASAP message log |

Implementation: `src/asap/manifest.c`, `src/gateway/routes.c`. If keys cannot load, manifest route returns **500** and agent startup fails fast (`init_subsystems()`).

Local capture for release publish:

```bash
make shellclaw
./scripts/dump_manifest.sh -o /tmp/manifest.json
```

---

## Public URL strategy (Q-URL)

v1.0 uses a **static** manifest on **GitHub Pages** — not a live tunnel to your home Jetson.

| URL | Role |
|-----|------|
| `https://asap-protocol.github.io/shellclaw/manifest.json` | **Canonical** signed manifest after `v*` tag (marketplace Manifest URL) |
| `https://shellclaw.example.com/asap` | Placeholder `endpoints.asap` in manifest (no live ASAP HTTP in v1.0) |
| `http://127.0.0.1:18789/.well-known/asap/manifest.json` | Dev gateway (dump script / local testing) |

IssueOps registration sets **`online_check: false`** so the registry does not probe a live agent. Marketplace shows a **Demo** badge for discoverable static manifests.

Live HTTPS ASAP endpoint (Tailscale Funnel, Cloudflare Tunnel) is documented under [Deferred (v1.0.1+)](#deferred-v101) below.

---

## Signature verification

ShellClaw signs manifests with **Ed25519** (TweetNaCl in C). Inner manifest JSON is canonicalized with **JCS (RFC 8785)** before signing (`src/crypto/jcs.c`).

### Key files

| Path | Mode | Notes |
|------|------|-------|
| `~/.shellclaw/keys/ed25519.priv` | `0600` | 64-byte secret; agent refuses startup if group/other bits set |
| `~/.shellclaw/keys/ed25519.pub` | `0600` on create | 32-byte public key |

Rotate: `./build/shellclaw --rotate-keys` (backs up prior keys with timestamp suffix).

### Verify published manifest (Python — recommended for operators)

After GitHub Pages deploy:

```bash
curl -fsS https://asap-protocol.github.io/shellclaw/manifest.json | python -m asap.crypto.verify_manifest
```

Or with a saved file:

```bash
curl -fsS https://asap-protocol.github.io/shellclaw/manifest.json -o /tmp/manifest.json
python3 -m asap.crypto.verify_manifest /tmp/manifest.json
```

### Verify locally (gateway running)

```bash
curl -fsS http://127.0.0.1:18789/.well-known/asap/manifest.json -o /tmp/live.json
python3 -m asap.crypto.verify_manifest /tmp/live.json
```

Schema-only check (CI-safe skip if `asap` package missing):

```bash
./scripts/validate_manifest.sh /tmp/manifest.json
```

**v1.0 note:** upstream `asap-compliance` harness may report `SignatureVerificationError` against the local gateway until JCS/signing is byte-identical with Python reference (planned v1.0.1+). C unit tests round-trip in `tests/test_manifest.c`.

---

## Marketplace registration (IssueOps)

ShellClaw v1.0 is listed in the public ASAP registry via the upstream **Register Agent** IssueOps template. The bot merges your form input with fields derived from the **signed manifest** at the manifest URL you provide.

### Published manifest URL (Q-URL)

After a release tag (`v*`), GitHub Pages publishes the live `SignedManifest` captured at build time:

**https://asap-protocol.github.io/shellclaw/manifest.json**

Verify before submitting IssueOps:

```bash
curl -fsS https://asap-protocol.github.io/shellclaw/manifest.json | python -m asap.crypto.verify_manifest
```

### IssueOps link

Open a new registration issue (do not use the auto-registration API for v1.0 — it runs compliance against a reachable live endpoint):

**https://github.com/asap-protocol/asap-protocol/issues/new?template=register_agent.yml**

### What to type into IssueOps

Use the table below. These are the **only** fields you enter manually. Do **not** type `hardware_class`, `inference_modes`, or `hardware_io` — upstream `derive_registry_hardware_fields()` copies them from your signed manifest after the bot fetches and validates it.

| IssueOps field | Value for ShellClaw v1.0 |
|----------------|--------------------------|
| **Name** | `ShellClaw` |
| **Description** | `The first C-native edge-AI-capable ASAP agent. Runs Phi-3-mini locally on NVIDIA Jetson Orin Nano Super via CUDA, exposes GPIO and I2C primitives on the 40-pin header as LLM-callable tools, and participates in the ASAP ecosystem with Ed25519-signed manifests.` |
| **Manifest URL** | `https://asap-protocol.github.io/shellclaw/manifest.json` |
| **HTTP endpoint** | `https://shellclaw.example.com/asap` (placeholder — no live ASAP endpoint in v1.0; static manifest only per Q-URL) |
| **Skills** (CSV) | `assistant,edge_briefing,server_admin,gpio_control` |
| **Category** | `Infrastructure` |
| **Built with** | `Other` |
| **Tags** (CSV) | `cuda,edge-ai,hardware,jetson,local-inference` |

**Repository** and **documentation** URLs are typically inferred from the manifest or repo metadata; confirm they match `https://github.com/asap-protocol/shellclaw` and `https://github.com/asap-protocol/shellclaw#readme` on the merged entry.

#### Do not submit manually

- **`hardware_class`**, **`inference_modes`**, **`hardware_io`** — derived from `capabilities.hardware` and `capabilities.inference` in the signed manifest (e.g. Jetson: `edge_accelerator`, `["cloud","local_cuda"]`, `["gpio","i2c"]`).
- **`self-signed` in tags** — upstream `anti_spam.py` adds the trust tag automatically; including it in your CSV can cause rejection or duplication.

#### Skills: v1.0 allowed vs deferred

IssueOps skills CSV must match manifest `capabilities.skills[].id` for v1.0:

| Skill | v1.0 |
|-------|------|
| `assistant` | Yes |
| `edge_briefing` | Yes |
| `server_admin` | Yes |
| `gpio_control` | Yes |
| `sensor_read` | No — Phase 7 (v1.2) |
| `camera_capture` | No — Phase 7 (v1.2) |
| `home_monitor` | No — Phase 7 (v1.2) |
| `visual_monitor` | No — Phase 7 (v1.2) |

Manifest code registers skills from `skills/*.md` on disk; default descriptions in `src/asap/manifest_build.c` align with the four IDs above. **Discovery cap:** at most **64** skills appear in `capabilities.skills` on `/.well-known/asap/manifest.json`; additional on-disk skills remain available via the authenticated `/api/skills` API at runtime.

#### `online_check` and Demo badge

v1.0 uses a **static** manifest URL only (no tunnel / live ASAP HTTP in production). The registry entry should have **`online_check: false`**. The marketplace UI shows a **Demo** badge (not Offline) for agents that are discoverable via manifest but not probed for liveness — see upstream `docs/guides/shellclaw-registry.md` §5.4.

### Reference JSON (after bot processing)

After IssueOps merges, `registry.json` should match the post-bot shape in:

- **In-repo copy:** [`docs/fixtures/shellclaw-v1.0-registry-entry.json`](fixtures/shellclaw-v1.0-registry-entry.json)
- **Upstream fixture:** [asap-protocol/asap-protocol `tests/fixtures/registry/shellclaw-v1.0-entry.json`](https://github.com/asap-protocol/asap-protocol/blob/main/tests/fixtures/registry/shellclaw-v1.0-entry.json)

Diff your live listing against that file to confirm `hardware_class`, `inference_modes`, and `hardware_io` were derived correctly.

URN (not typed in IssueOps; set by bot/manifest): **`urn:asap:agent:shellclaw`** (DR-021 / Q-URN).

---

## Operator kit (task 6.3)

In-repo artifacts reduce submission friction; **live marketplace listing** remains an operator step tracked in [`docs/MARKETPLACE_STATUS.md`](MARKETPLACE_STATUS.md).

| Artifact | Purpose |
|----------|---------|
| [`docs/issueops/register-agent-prefill.md`](issueops/register-agent-prefill.md) | IssueOps link + copy-paste form values + pre/post gates |
| [`docs/issueops/VERIFY_MARKETPLACE.md`](issueops/VERIFY_MARKETPLACE.md) | Post-submit Browse UI / Demo badge / filter verification |
| [`scripts/open_marketplace_registration.sh`](../scripts/open_marketplace_registration.sh) | Prints URL + doc paths (no network) |

### Checklist for human submission (task 6.3)

Complete **after** Wave 5 signing is live on GitHub Pages (task 6.1) and **before** the `v1.0.0` tag if possible. Check off in `MARKETPLACE_STATUS.md` when done.

- [ ] `curl` manifest URL; `verify_manifest` succeeds
- [ ] Open [Register Agent](https://github.com/asap-protocol/asap-protocol/issues/new?template=register_agent.yml) and fill fields from the table above (or prefill doc)
- [ ] Skills CSV is exactly four v1.0 skills (no sensor/camera/monitor skills)
- [ ] Tags CSV does **not** include `self-signed`
- [ ] Did **not** manually add `hardware_class`, `inference_modes`, or `hardware_io` in the issue body
- [ ] Wait for bot merge; confirm listing on marketplace Browse UI
- [ ] Agent detail shows **Demo** badge; filters show `edge_accelerator`, `local_cuda`, `gpio` / `i2c` from derived fields

---

## Deferred (v1.0.1+)

Planned for **v1.0.1** after v1.0 ships with static manifest only (Q-URL). Task 6.4 is documentation-only in v1.0.

### Live ASAP HTTP endpoint (task 6.4)

v1.0 does not expose a reachable `endpoints.asap` URL for cross-agent invocation or auto-registration compliance. To enable it yourself before v1.0.1:

1. **Gateway** — Run ShellClaw with gateway enabled; set `[asap] public_base_url` to your public origin (HTTPS).
2. **Tunnel** — Terminate TLS at the edge with one of:
   - **[Tailscale Funnel](https://tailscale.com/kb/1223/tailscale-funnel)** — expose local port 443/8080 to the tailnet/public funnel hostname; map to gateway ASAP route.
   - **[Cloudflare Tunnel](https://developers.cloudflare.com/cloudflare-one/connections/connect-apps/)** — `cloudflared tunnel` to `localhost:<gateway-port>`; use a Cloudflare-managed hostname in `public_base_url`.
3. **Manifest** — Update manifest `endpoints.asap` to the public ASAP path; re-publish signed manifest (GitHub Pages or agent well-known).
4. **Marketplace** — Re-submit or update registry entry so `online_check` can be true; run compliance harness (task 11.0).

Security: exposing the agent increases attack surface — complete gateway hardening and audit before production funnel use (see slice risks in planning doc).

### Other v1.0.1+ items

- **ASAP compliance harness** — see [ASAP compliance harness](#asap-compliance-harness) below; target green run in v1.0.1 after live endpoint + schema alignment.

---

## ASAP compliance harness

ShellClaw v1.0 ships a **static** signed manifest on GitHub Pages (Q-URL) and a **partial** local gateway (manifest + minimal health + `/asap` stub). The upstream [`asap-compliance`](https://pypi.org/project/asap-compliance/) pytest harness (PRD §4.12) is the reference for a **reachable live agent**. v1.0 accepts documented deviations instead of a green badge.

### Manual procedure (contributors)

Re-run after any change to `src/asap/`, `src/gateway/routes.c`, or manifest signing (`src/crypto/`). **Not** a v1.0 release gate — for pre-release iteration only.

1. **Build** — `make shellclaw`
2. **Start gateway** — either:
   - Existing install: `systemctl --user start shellclaw` with `[gateway] enabled`, **or**
   - Ephemeral: `./scripts/dump_manifest.sh` (boots short-lived gateway on `127.0.0.1:18789`)
3. **Health** — `curl -sf http://127.0.0.1:18789/health`
4. **Manifest shape** — `curl -sf http://127.0.0.1:18789/.well-known/asap/manifest.json | head -c 200`
5. **Run harness**:

```bash
python3 -m venv .venv-asap && source .venv-asap/bin/activate
./scripts/run_asap_compliance.sh
# or explicit URL:
./scripts/run_asap_compliance.sh http://127.0.0.1:18789
```

6. **Compare failures** to the [documented deviations](#documented-deviations) table below. New failures after a gateway change need a row update or a code fix.
7. **Optional schema check** — pipe unsigned inner manifest or use `validate_manifest.sh` on `manifest_build_json` output in tests.

Requires **Python 3.13+** recommended and network for first `pip install asap-compliance` (unless `ASAP_COMPLIANCE_SKIP_PIP=1`).

### How to run (reference)

From the repository root, with the gateway listening (default `http://127.0.0.1:18789` per `config.example.toml`):

```bash
make shellclaw
# Start shellclaw with [gateway] enabled, or use an ephemeral instance:
#   ./scripts/dump_manifest.sh -o /tmp/manifest.json   # boots gateway, then exits

python3 -m venv .venv-asap && source .venv-asap/bin/activate   # PEP 668 / macOS
./scripts/run_asap_compliance.sh
./scripts/run_asap_compliance.sh http://127.0.0.1:18789
ASAP_AGENT_URL=http://127.0.0.1:18789 ./scripts/run_asap_compliance.sh
```

| Environment variable | Purpose |
|---------------------|---------|
| `ASAP_AGENT_URL` | Agent base URL (default `http://127.0.0.1:18789`) |
| `ASAP_COMPLIANCE_SKIP_PIP=1` | Skip `pip install` when `asap-compliance` is already installed |
| `ASAP_COMPLIANCE_PIP_USER=1` | Use `pip install --user` |
| `ASAP_COMPLIANCE_VERSION` | Pip spec (default `asap-compliance>=1.0.0`) |
| `ASAP_COMPLIANCE_SKIP_HEALTH=1` | Skip curl probe of `/health` before pytest |
| `ASAP_COMPLIANCE_LIVE_TEST_DIR` | Directory for generated live pytest (default: temp) |

The script installs `asap-compliance`, probes `GET $ASAP_AGENT_URL/health`, then runs three live tests (`test_live_handshake`, `test_live_state_machine`, `test_live_sla`) generated under a temp dir. It does **not** start the agent; use `scripts/dump_manifest.sh` for a short-lived local gateway during manifest publish.

**Last harness run (2026-05-24):** `asap-compliance` 1.2.0, Python 3.14 venv `.venv-asap`, agent `http://127.0.0.1:18789` — **3 failed**, 0 passed (see deviations table).

### v1.0 stance

| Area | v1.0 | v1.0.1+ |
|------|------|---------|
| Marketplace discovery | Static `SignedManifest` on GitHub Pages | Optional live `endpoints.asap` + tunnel |
| Compliance badge | Deviations documented here (PRD §4.12 OR) | Run harness green against public URL |
| Gateway ASAP route | Manifest + health stubs; `/asap` parses legacy flat `params` | Align with harness `params.envelope`, task handlers, `HealthStatus` |
| Manifest trust | Ed25519 + JCS in C; unit tests pass | Byte-identical verification with upstream `asap` Python |

### Documented deviations

Failures observed from `./scripts/run_asap_compliance.sh` against a local gateway with `[gateway] enabled` and a signed manifest served at `/.well-known/asap/manifest.json`.

| Test / check | Observed | Expected (harness / upstream) | Reason | Planned fix |
|--------------|----------|-------------------------------|--------|-------------|
| `test_live_handshake` → `health_schema` | `GET /.well-known/asap/health` → `{"status":"ok"}` | `HealthStatus`: `status` (`healthy`/`unhealthy`), `agent_id`, `version`, `uptime_seconds`, optional `asap_version`, `load` | v1.0 minimal stub in `manifest_health_json()`; not wired to agent URN, release semver, or uptime | **v1.0.1+** — emit full `HealthStatus` from config + `board_detect` |
| `test_live_handshake` → `manifest_signature` | `validate_signed_manifest_response(..., verify_signature=True)` raises `SignatureVerificationError` | Ed25519 over JCS-canonical inner `manifest` verifies with upstream `asap` crypto | C-side RFC 8785 JCS subset (`src/crypto/jcs.c`) + TweetNaCl sign path not yet byte-identical with Python verifier used by harness; C unit tests round-trip locally | **v1.0.1+** — align JCS + signing with upstream reference; keep `tests/test_manifest.c` + `scripts/validate_manifest.sh` |
| `test_live_handshake` → `version_reported` | Skipped (manifest not trusted) | ASAP protocol version compatibility check after manifest parse | Cascades from `manifest_signature` failure | **v1.0.1+** — after signature fix |
| `test_live_state_machine` → `state_agent_response` | `POST /asap` → HTTP **400** (`Invalid params: missing or bad type for 'id'`) | HTTP **200** JSON-RPC result with `task.response` envelope | Harness sends `params.envelope` + `idempotency_key` (no top-level envelope `id` in nested object); ShellClaw `asap_envelope_parse` reads **flat** envelope fields directly under `params` (legacy client shape in `asap_envelope_to_jsonrpc_request`) | **v1.0.1+** — accept `params.envelope`, generate/validate envelope `id`, implement compliance `echo` skill (or map harness skill id) |
| `test_live_sla` → `sla_task_timeout` | `POST /asap` → HTTP **400** (same as state) | HTTP **200** within SLA window for `sla_skill_id` (`echo`) | Same JSON-RPC `params` shape mismatch; SLA cannot reach agent handler | **v1.0.1+** — same as state machine row |
| `test_live_sla` → `sla_progress_schema` | Passed (static schema check) | Valid `TaskUpdate` progress schema | Does not require live agent success | No change for v1.0 |
| Gateway `GET /health` (script preflight only) | `{"status":"ok","uptime":…,"version":"…"}` (gateway build label) | Not used by harness (harness uses `/.well-known/asap/health` only) | Separate ShellClaw gateway health route; harmless for harness | Optional alignment in v1.0.1+ if operators want one schema everywhere |

**Note:** `/api/status` and provider health are out of scope for the ASAP harness; only well-known ASAP paths and `POST /asap` are tested.

### Static manifest vs live compliance

IssueOps registration (task 6.3) intentionally uses **static** manifest URL with `online_check: false` so the marketplace does not run Compliance Harness v2 against a live endpoint. Local `./scripts/run_asap_compliance.sh` is for **pre-release iteration** on a dev gateway; a green run is **not** a v1.0 release gate.

---

## Related docs

| Doc | Topic |
|-----|-------|
| [ARCHITECTURE.md](ARCHITECTURE.md) | Module map, gateway, thread safety |
| [SECURITY.md](SECURITY.md) | Key file permissions, gateway auth |
| [LOCAL_INFERENCE.md](LOCAL_INFERENCE.md) | llama-server (out of ASAP scope) |
| [issueops/register-agent-prefill.md](issueops/register-agent-prefill.md) | Copy-paste IssueOps values |
| [MARKETPLACE_STATUS.md](MARKETPLACE_STATUS.md) | Operator checklist tracking |
