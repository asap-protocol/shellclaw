# Verify marketplace listing (task 6.3)

Short operator runbook after IssueOps registration for ShellClaw v1.0. Prefill and gates: [`register-agent-prefill.md`](register-agent-prefill.md). Tracking: [`../MARKETPLACE_STATUS.md`](../MARKETPLACE_STATUS.md).

## Prerequisites

- [ ] GitHub Pages manifest URL returns 200: `https://asap-protocol.github.io/shellclaw/manifest.json`
- [ ] `python3 -m asap.crypto.verify_manifest` succeeds on that URL (pre-submit gate)
- [ ] Register Agent issue filed and bot merge completed (issue URL recorded in `MARKETPLACE_STATUS.md`)

## Verify criteria (task 6.3)

| Check | Pass criterion |
|-------|----------------|
| Browse UI | **ShellClaw** appears in marketplace agent list |
| Detail page | Agent opens; name/description match IssueOps submission |
| Demo badge | Detail shows **Demo** (static manifest, no liveness probe) — not **Offline** |
| Derived hardware | Filters or detail show `hardware_class` = `edge_accelerator` |
| Derived inference | `inference_modes` includes `local_cuda` (and `cloud`) |
| Derived I/O | `hardware_io` includes `gpio` and `i2c` |
| Skills | Exactly four: `assistant`, `edge_briefing`, `server_admin`, `gpio_control` |
| Registry shape | Live entry matches [`docs/fixtures/shellclaw-v1.0-registry-entry.json`](../fixtures/shellclaw-v1.0-registry-entry.json) for derived fields and `online_check: false` |

## Quick diff (optional)

If you have a checkout of `asap-protocol/asap-protocol` with the fixture:

```bash
# Compare key derived fields (adjust path to your clone)
jq '{hardware_class,inference_modes,hardware_io,online_check,skills}' \
  docs/fixtures/shellclaw-v1.0-registry-entry.json
```

Compare output to what the marketplace UI or exported registry JSON shows for ShellClaw.

## Failure notes

- **Offline instead of Demo** — confirm `online_check: false` on the entry; v1.0 is manifest-only per Q-URL.
- **Missing filter tags** — bot may not have fetched/validated manifest; re-check manifest URL and signature, re-open IssueOps if needed.
- **Wrong skills** — IssueOps CSV must match manifest `capabilities.skills[].id`; do not declare Phase 7 skills in v1.0.

## Sign-off

When all rows pass, set `docs/MARKETPLACE_STATUS.md` status to **listed** and check off the verification checklist there.
