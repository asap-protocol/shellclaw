# Local briefing with optional cloud fallback

Produce scheduled status briefings when a cron job injects a message asking for a briefing (for example: "morning briefing", "daily edge briefing", "status briefing").

## Cron setup

Use the `cron` tool to register recurring jobs. Schedule formats:

- `interval:N` — every N seconds
- `at:UNIX_TS` — one-shot at Unix timestamp (job auto-deletes after run)
- `cron:MIN HOUR DOM MONTH DOW` — five-field cron (`*` or ranges; DOW 0–6, Sunday = 0)

Example create payload:

```json
{
  "operation": "create",
  "schedule": "cron:0 8 * * *",
  "message": "morning briefing: summarize host and edge health",
  "channel": "cli",
  "recipient": "default"
}
```

Target the operator's active channel (`telegram`, `discord`, `web`, or `cli`) when known.

## Briefing workflow

1. Gather facts with read-only tools before writing the narrative:
   - `shell` — `uptime`, `df -h`, `free -h`, `systemctl --user is-active shellclaw llama-server` (adjust if services differ)
   - Optional: `i2c_scan` on the configured bus when hardware is enabled (empty array is valid — no sensors wired)
   - Optional: `gpio_read` on a known monitoring pin only when configured
2. Compose a briefing under 400 words unless the user asks for detail.
3. Structure: **Host** (uptime, disk, memory) → **Services** (agent, local LLM) → **Hardware** (I2C/GPIO summary if checked) → **Actions** (one suggested follow-up, if any).

## Cloud vs local inference

ShellClaw routes LLM calls through `fallback_chain` (default: cloud providers, then `local`). When cloud APIs are down, rate-limited, or keys are unset, the router activates the local `llama-server` provider.

For briefings:

- Prefer factual, tool-grounded sentences so local models hallucinate less.
- If the active provider is local (smaller model), shorten further: bullet list, no filler.
- Do not claim cloud model quality when running locally — note "generated on-device" when fallback occurred or when `/api/status` would show `local` as active.

## Constraints

- No sensor decoders (BME280, BH1750, DHT22) — deferred to v1.2.
- No `camera_capture` in routine briefings — deferred to v1.2.
- Do not create or delete cron jobs inside a briefing response unless the user explicitly asked to change the schedule.
