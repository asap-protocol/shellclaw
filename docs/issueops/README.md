# IssueOps templates (v1.0.0)

| File | Use |
|------|-----|
| [`v1.0.0-jetson-signoff-issue.md`](v1.0.0-jetson-signoff-issue.md) | Optional body for **v1.0.0 Jetson sign-off** (known pending; not a merge gate) |
| [`pr-development-to-main-v1.0.0.md`](pr-development-to-main-v1.0.0.md) | Body for PR **Phase 5 onto `main`** |
| [`register-agent-prefill.md`](register-agent-prefill.md) | ASAP marketplace registration (after `v1.0.0` tag) |
| [`VERIFY_MARKETPLACE.md`](VERIFY_MARKETPLACE.md) | Post-registration marketplace checks |

```bash
# Jetson tracking issue (optional — known pending, not a merge gate)
gh issue create --repo asap-protocol/shellclaw \
  --title "v1.0.0 Jetson sign-off" \
  --body-file docs/issueops/v1.0.0-jetson-signoff-issue.md

# Release PR (draft)
gh pr create --repo asap-protocol/shellclaw --base main --head development \
  --title "release: v1.0.0 edge Jetson foundation" \
  --body-file docs/issueops/pr-development-to-main-v1.0.0.md --draft
```
