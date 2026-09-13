# Hardware safety (Jetson + Raspberry Pi)

Electrical safety guide for the **40-pin expansion header** on NVIDIA Jetson Orin Nano Super and Raspberry Pi Zero 2 W. ShellClaw GPIO/I2C tools assume you have read this document. Applies to both boards unless noted.

For Jetson-specific pin maps and wiring examples, see [HARDWARE_JETSON.md](HARDWARE_JETSON.md).

---

## Logic levels — 3.3 V only

| Rule | Detail |
|------|--------|
| GPIO voltage | **3.3 V** logic on all GPIO and I2C pins |
| **Not 5 V tolerant** | Driving 5 V into a GPIO pin can **permanently damage** the SoC |
| 5 V pins | Physical pins labeled **5V** (pins 2 and 4) are power outputs — never connect them to GPIO |
| 3.3 V power | Pin 1 and pin 17 are **3V3** supply rails — see current limits below |

When interfacing 5 V peripherals (Arduino Uno, many relay boards, legacy TTL):

- Use a **level shifter** (3.3 V ↔ 5 V) or an **open-collector** buffer with pull-up to 3.3 V — not to 5 V on the SBC side.
- Prefer **3.3 V-native modules** (most modern I2C breakouts).

I2C on both boards is **3.3 V**. Do not connect I2C directly to a 5 V bus.

---

## Current limits

### Per GPIO pin

| Board | Typical max source/sink per pin |
|-------|----------------------------------|
| Raspberry Pi (BCM) | ~**16 mA** per pin; **50 mA** total across all GPIO (soft guideline) |
| Jetson Orin Nano header | Treat as **low current** (~**8–16 mA** per pin); NVIDIA docs emphasize limited drive strength |

**Never** drive motors, solenoids, high-power LEDs, or relay coils directly from a GPIO pin.

Use a **transistor, MOSFET, or dedicated driver board** with separate power for the load. Flyback protection (diode) for inductive loads.

### 3.3 V rail (pins 1 and 17)

The onboard 3.3 V regulator supplies the header **plus** on-board logic.

| Guideline | Value |
|-----------|-------|
| Budget for **your** peripherals | Stay under **~500 mA** combined on 3.3 V from the header unless the carrier board datasheet specifies higher |
| High-current sensors / radios | Power from a **separate regulated 3.3 V supply** with **common ground** to the SBC |
| USB peripherals | Use USB ports for cameras/drives — not the 3.3 V pin |

Drawing too much current causes brownouts, SD/NVMe corruption, or thermal shutdown.

### 5 V rail (pins 2 and 4)

5 V pins mirror input power (USB-C on Jetson, micro-USB/USB-C on Pi). Limited current available for hats/modules — check carrier/PSU rating. Do not back-feed the board from the 5 V pins.

---

## ESD and handling

Static discharge can destroy GPIO and I2C inputs without visible damage.

| Practice | Why |
|----------|-----|
| Touch grounded chassis before handling boards | Discharge body capacitance |
| Work on a **ESD mat** with wrist strap when wiring regularly | Keeps potentials equalized |
| Insert/remove HATs and jumper wires with **power off** | Prevents latch-up and shorts |
| Store boards in **anti-static bags** | Prevent latent ESD damage |
| Avoid touching pin headers or IC pins directly | Fingers add ESD and oil contamination |

In dry environments, ESD risk increases — extra caution recommended.

---

## Wiring checklist

Before applying power:

1. **Continuity** — no short between 3V3, 5V, and GND.
2. **Pin map** — physical pin numbers match [Jetson](HARDWARE_JETSON.md) or Pi pin table (`src/hardware/boards/rpi_zero2w.h`); SFIO pins are not GPIO on Jetson without pinmux changes.
3. **Ground first** — connect GND before signal lines when prototyping.
4. **I2C** — SDA/SCL not swapped; pull-ups present (on module or breadboard).
5. **Camera ribbon** — seated with correct orientation; power off when reseating.

After wiring:

```bash
# Jetson
gpiodetect && gpioinfo gpiochip0
sudo i2cdetect -y 7

# Raspberry Pi (Phase 6+)
gpiodetect && sudo i2cdetect -y 1
```

Start with **read-only** tools (`gpio_read`, `i2c_scan`) before `gpio_write`.

---

## Software limits (ShellClaw)

These do **not** replace hardware safety above:

| Control | Behavior |
|---------|----------|
| SFIO rejection | GPIO tools refuse I2C/UART/SPI pins still in SFIO mode |
| Sandbox | Shell tool runs isolated; hardware access only via authenticated agent tools |
| Camera spawn | Fixed argv only — no shell interpolation of user paths |
| Gateway auth | `/api/hardware/*` requires Bearer token |

Shell sandbox commands also use a **substring deny list** for dangerous paths (including Jetson GPU device nodes). Innocent command text that contains those substrings may be blocked even when no real device access was intended. See [SECURITY.md](SECURITY.md) § Linux sandbox (Jetson) and residual-risk notes.

Mis-wiring can still damage hardware before software runs. **Power off** when unsure.

---

## When to stop and ask for help

Stop and review wiring if you observe:

- Board resets when toggling GPIO
- Smoke, hot components, or burning smell (**remove power immediately**)
- I2C bus stuck (all addresses respond on `i2cdetect`)
- GPIO reads random values with nothing connected (possible damaged pin)

---

## Related docs

- [HARDWARE_JETSON.md](HARDWARE_JETSON.md) — Jetson pin map, I2C bus 7, camera
- [ARCHITECTURE.md](ARCHITECTURE.md) — hardware software layer
- [SECURITY.md](SECURITY.md) — gateway and sandbox boundaries
