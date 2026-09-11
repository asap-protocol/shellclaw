# Register Agent — IssueOps prefill (ShellClaw v1.0)

Operator copy-paste kit for [task 6.3](../../.cursor/dev-planning/tasks/phase5/03-inference-trust-marketplace.md). Full context: [`docs/ASAP.md`](../ASAP.md).

## Direct link

**https://github.com/asap-protocol/asap-protocol/issues/new?template=register_agent.yml**

## Copy-paste block (form fields)

Paste each value into the matching field in the Register Agent issue form. Do **not** add `hardware_class`, `inference_modes`, `hardware_io`, or `self-signed` in tags — upstream derives the first three from your signed manifest and adds the trust tag automatically.

```
Name: ShellClaw

Description: The first C-native edge-AI-capable ASAP agent. Runs Phi-3-mini locally on NVIDIA Jetson Orin Nano Super via CUDA, exposes GPIO and I2C primitives on the 40-pin header as LLM-callable tools, and participates in the ASAP ecosystem with Ed25519-signed manifests.

Manifest URL: https://asap-protocol.github.io/shellclaw/manifest.json

HTTP endpoint: https://shellclaw.example.com/asap

Skills (CSV): assistant,edge_briefing,server_admin,gpio_control

Category: Infrastructure

Built with: Other

Tags (CSV): cuda,edge-ai,hardware,jetson,local-inference
```

After the bot merges, confirm **repository** and **documentation** URLs match `https://github.com/asap-protocol/shellclaw` and `https://github.com/asap-protocol/shellclaw#readme` on the listing (usually inferred from manifest/repo metadata).

## Pre-submission gates

Complete **before** opening the issue (ideally after GitHub Pages publish from a `v*` tag — task 6.1).

1. **Manifest URL live**

   ```bash
   curl -fsS https://asap-protocol.github.io/shellclaw/manifest.json -o /tmp/shellclaw-manifest.json
   ```

   Expect HTTP 200 and valid JSON (`SignedManifest` wrapper).

2. **Cryptographic verify** (requires `pip install asap-protocol` or equivalent env with `asap`):

   ```bash
   curl -fsS https://asap-protocol.github.io/shellclaw/manifest.json | python3 -m asap.crypto.verify_manifest
   ```

   Expect exit code 0 (no error output).

3. **Optional schema check** (local file after curl):

   ```bash
   ./scripts/validate_manifest.sh /tmp/shellclaw-manifest.json
   ```

   Note: `validate_manifest.sh` validates the inner `Manifest` shape when given bare manifest JSON; for the published **SignedManifest**, use `verify_manifest` as the gate.

4. **Skills policy** — CSV must be exactly four IDs: `assistant`, `edge_briefing`, `server_admin`, `gpio_control` (no Phase 7 skills).

5. **Tags** — do **not** include `self-signed` in your CSV.

## Post-submission verification

After the IssueOps bot merges your registration:

1. Open the ASAP marketplace **Browse** UI and find **ShellClaw**.
2. Open the agent **detail** page — expect a **Demo** badge (not Offline); `online_check` should be false for static manifest-only v1.0.
3. Use marketplace **filters**: `edge_accelerator`, `local_cuda`, and GPIO/I2C derived from manifest (`hardware_io`: `gpio`, `i2c`).
4. Diff the live registry entry against [`docs/fixtures/shellclaw-v1.0-registry-entry.json`](../fixtures/shellclaw-v1.0-registry-entry.json) (or upstream `tests/fixtures/registry/shellclaw-v1.0-entry.json`).

Record the submitted issue URL in [`docs/MARKETPLACE_STATUS.md`](../MARKETPLACE_STATUS.md).

## Helper script (no network)

```bash
./scripts/open_marketplace_registration.sh
```

Prints the IssueOps URL and path to this prefill doc.

## Submission method

Registration is a **human** step via the GitHub issue template above. Do not use `POST /registry/agents` auto-registration for v1.0 (compliance harness expects a reachable live endpoint). Automated `gh issue create` against `asap-protocol/asap-protocol` is optional and not required for slice completion — use the web form with the prefill block.
