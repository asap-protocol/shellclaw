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
| `GET /.well-known/asap/health` | Public | Agent liveness, manifest identity and version, and gateway uptime |
| `POST /asap` | Rate-limited | Accepts flat legacy messages and SDK echo tasks. Requests have a 1 MiB limit. Responses that exceed the 64 KiB gateway buffer return HTTP 500 with JSON-RPC error `-32603`. |
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

The live compliance checks verify the gateway's Ed25519 signature with the Python SDK.

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

Manifest code registers skills from `skills/*.md` on disk; default descriptions in `src/asap/manifest_build.c` align with the four IDs above. **Discovery cap:** the manifest lists the built-in `echo` skill and up to **64** installed skills on `/.well-known/asap/manifest.json`; additional on-disk skills remain available via the authenticated `/api/skills` API at runtime.

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

## SDK tasks

The Python SDK sends requests to `POST /asap`. Put a `TaskRequest` in `params.envelope`. The recipient must match `[asap].agent_urn`. If you configure `[asap].trusted_senders`, the gateway also checks the sender URN against that list.

The manifest advertises the built-in `echo` skill once. The skill returns the input object as `TaskResponse.result`. Repeating an echo request produces the same result without using the model, local tools, or conversation history.

The gateway rejects other SDK skill IDs before the model runs. Use the legacy flat request format for tasks that run the agent or call local tools.

SDK replies use `result.envelope`. The reply's `correlation_id` contains the request envelope ID. Both SDK and legacy replies echo the top-level JSON-RPC ID when the gateway can read it. Legacy requests use flat `params` and receive flat `result` envelopes.

The sender list compares a URN supplied by the client. Control network access to the legacy route because it uses that list for agent and tool requests.

## ASAP compliance harness

CI runs the handshake, task state, and SLA validators from `asap-compliance==1.4.0` with `asap-protocol==2.6.0` on Python 3.14. The job starts a real gateway on loopback with a temporary home and a stub provider. All three tests must pass. The SLA check sends an `echo` task and measures it against a five-second timeout.

To run the same checks locally, build the gateway and install the pinned packages in a Python 3.14 virtual environment.

```bash
make GATEWAY=1 shellclaw
python3.14 -m venv .venv-asap
source .venv-asap/bin/activate
python3 -m pip install 'asap-compliance==1.4.0' 'asap-protocol==2.6.0'
python3 scripts/run_asap_ci.py build/shellclaw
```

For a running test gateway, use `ASAP_COMPLIANCE_SKIP_PIP=1 ./scripts/run_asap_compliance.sh http://127.0.0.1:18789`. The upstream validator sends from `urn:asap:agent:compliance-harness` to `urn:asap:agent:under-test`. Set those identities in the test gateway configuration.

The marketplace entry uses a static signed manifest. The CI checks exercise the local gateway.
