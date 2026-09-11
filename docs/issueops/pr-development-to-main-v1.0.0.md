# PR body — `development` → `main` (Phase 5 onto main)

**Title:** `release: land Phase 5 on main (Jetson on-device pending)`

**Create PR:**

```bash
gh pr create --repo asap-protocol/shellclaw \
  --base main \
  --head development \
  --title "release: land Phase 5 on main (Jetson on-device pending)" \
  --body-file docs/issueops/pr-development-to-main-v1.0.0.md
```

---

## Summary

Land Phase 5 on **`main`**: GPIO/I2C tools, CUDA local inference path, signed ASAP manifest, gateway `/hardware` UI (sensor/camera panels deferred to v1.2).

**Known pending (not a merge gate):** on-device Jetson Orin Nano Super sign-off — [`docs/JETSON_SIGNOFF.md`](../JETSON_SIGNOFF.md). Continue product work on `main`; run the checklist when hardware is available.

## Evidence

| Artifact | Link |
|----------|------|
| Security self-audit | [`docs/SECURITY.md`](../SECURITY.md) |
| Benchmarks (Jetson rows still `_run on device_`) | [`docs/BENCHMARKS.md`](../BENCHMARKS.md) |
| Changelog | [`CHANGELOG.md`](../../CHANGELOG.md) § [1.0.0] / Unreleased |
| Jetson operator checklist | [`docs/JETSON_SIGNOFF.md`](../JETSON_SIGNOFF.md) |
| Release runbook | [`docs/RELEASE_V1.0.md`](../RELEASE_V1.0.md) |

## Jetson sign-off

- **Status:** known pending — does not block this PR
- **On-device runner (later):** `SHELLCLAW_HW_TEST=1 make test_hardware_on_device`
- **Manual checklist:** [`JETSON_SIGNOFF.md`](../JETSON_SIGNOFF.md)

## Pre-merge verification (x86 / CI)

- [ ] `CI=true GATEWAY=1 make clean && CI=true GATEWAY=1 make test`
- [ ] `make static` — zero cppcheck findings (when cppcheck is available)
- [ ] `make test-sanitize` — AddressSanitizer + UBSan (Linux CI)
- [ ] `make release` binary < 2 MB (CI); hardware backends target < 600 KB

## Post-merge (maintainer — not in this PR)

1. Continue work on `main`
2. Optional later: Jetson sign-off, then tag `v1.0.0` per [`RELEASE_V1.0.md`](../RELEASE_V1.0.md) Phase C
3. Pages manifest + marketplace IssueOps when tagging

## Test plan

- [ ] CI green on `development` at merge SHA
- [ ] No v1.2-deferred features claimed as shipped (sensors, camera E2E, deferred skills)
- [ ] Jetson on-device work tracked as known pending, not as a blocker
