# GPIO pin control

Control the 40-pin header with `gpio_read`, `gpio_write`, and `gpio_mode` only. Use the `server_admin` skill for shell diagnostics and `i2c_scan`.

## Tools

| Tool | Args | Notes |
|------|------|-------|
| `gpio_mode` | `pin`, `mode` (`input` or `output`) | Set direction before driving outputs. |
| `gpio_write` | `pin`, `value` (`0` or `1`) | Requires output mode. |
| `gpio_read` | `pin` | Physical header pin 1–40 (not SoC line numbers). |

## Flow

1. `gpio_mode` → output or input as needed.
2. `gpio_write` or `gpio_read`.
3. Report pin number and value clearly in the reply.

## Board notes

- **Jetson Orin Nano Super:** `gpiochip0` (Tegra GPIO) and `gpiochip1` (AON). Default test pin may be set in `config.example.toml` (`gpio_test_pin`).
- **Raspberry Pi Zero 2 W:** single header on `gpiochip0`.

## Safety

Follow [HARDWARE_SAFETY.md](../docs/HARDWARE_SAFETY.md): **3.3 V logic only**, limited current per pin, ESD precautions. Use external drivers for relays, motors, or loads above header ratings. Never short supply rails to GPIO.

## Scope

- No `shell`, `i2c_scan`, or sensor decoders — see `server_admin` and v1.2 skills for those.
- No `camera_capture` in this skill.
