# Host administration tasks

Operate the edge host with the sandboxed toolbox: `shell`, `i2c_scan`, `gpio_read`, `gpio_write`, and `gpio_mode`. This skill covers administration and bus/pin diagnostics — not environmental sensors or camera vision (v1.2).

## Tool reference

| Tool | Purpose |
|------|---------|
| `shell` | Read-only diagnostics first (`uptime`, `df`, `free`, `journalctl`, `systemctl --user status`). Mutating commands only when the user clearly requested them. |
| `i2c_scan` | Probe an I2C bus for 7-bit addresses. Args: optional `bus` (board default if omitted). Returns JSON array — empty array means no devices found, not an error. |
| `gpio_read` | Read a **physical** 40-pin header pin (1–40). Args: `pin`. |
| `gpio_write` | Drive HIGH/LOW. Args: `pin`, `value` (0 or 1). Set output mode first. |
| `gpio_mode` | Set direction. Args: `pin`, `mode` (`input` or `output`). |

Do **not** use `i2c_read` / `i2c_write` to interpret sensor registers — no decoders ship in v1.0. Do **not** use `camera_capture` for admin tasks.

## Recommended flow

1. **Inspect** — shell commands and `i2c_scan` before touching GPIO.
2. **Configure pin** — `gpio_mode` → `gpio_write` or `gpio_read`.
3. **Report** — JSON tool output summarized for the user; include pin numbers and values.

Jetson Orin Nano Super: `gpiodetect` typically shows `gpiochip0` (Tegra GPIO) and `gpiochip1` (AON). RPi Zero 2 W: single header on `gpiochip0`. I2C bus numbers are board-specific — use config defaults or ask the user before assuming.

## Safety (40-pin header)

- **3.3 V logic only** — do not drive 5 V into header pins.
- Respect current limits; use external drivers for relays, motors, or high-current loads.
- Never short power (3V3, 5V) to ground via GPIO.
- Prefer read-only shell and scan operations when troubleshooting; confirm before reboot, shutdown, or destructive filesystem operations.

## Sandbox

When `[sandbox] enabled = true`, shell commands pass allowlist checks and run in an isolated namespace without GPU or Argus socket access. If a command is blocked, explain the block and suggest a safer read-only alternative.

## Out of scope (v1.2)

- BME280 / BH1750 / DHT22 temperature, humidity, lux, or pressure readings
- CSI or USB camera capture and image description
- `home-monitor` / `visual-monitor` automation
