# Jetson Orin Nano Super — hardware guide

Operator guide for ShellClaw on the **NVIDIA Jetson Orin Nano Super 8 GB Dev Kit** (JetPack 6.2.x). GPIO and I2C primitives ship in **v1.0**; sensor decoders, camera image return to the LLM, and related Web UI panels ship in **v1.2** (Phase 7). See the [README roadmap](../README.md).

Shared electrical safety rules for the 40-pin header apply on Jetson and Raspberry Pi — read [HARDWARE_SAFETY.md](HARDWARE_SAFETY.md) before wiring anything.

---

## Bill of materials (v1.0 minimum)

| Item | Notes |
|------|-------|
| Jetson Orin Nano Super 8 GB Dev Kit | Primary v1.0 target |
| Active cooler | Required for sustained CUDA inference; verify fan spins at boot |
| microSD (64 GB+) or **NVMe SSD** | NVMe strongly recommended for model load and benchmarks |
| USB-C power supply (5V/3A+) | Use NVIDIA-approved or known-good PD adapter |
| Dupont jumpers, breadboard | For GPIO/I2C experiments |
| Optional: BME280, BH1750, IMX219 CSI module | **v1.2** — safe to buy early; not required for v1.0 sign-off |

**Intentionally not supported in v1.0:** DHT22 (deferred to v1.2 as **experimental** per decision Q-DHT22). Do not expect one-wire humidity reads until Phase 7.

---

## JetPack flash (6.2.x)

1. Download **JetPack 6.2.x** (L4T with kernel 5.15) from [NVIDIA Jetson Linux](https://developer.nvidia.com/embedded/jetson-linux).
2. Use **NVIDIA SDK Manager** on a host Ubuntu machine, or flash from CLI with `flash.sh` per NVIDIA docs.
3. First boot: complete OEM setup, create user, connect network.
4. Verify:

```bash
cat /etc/nv_tegra_release          # JetPack / L4T version
uname -r                           # expect 5.15.x-tegra
```

5. Install dev packages ShellClaw expects:

```bash
sudo apt update
sudo apt install -y libgpiod-dev i2c-tools v4l-utils \
  gstreamer1.0-tools gstreamer1.0-plugins-good
```

Add your user to the `gpio` and `i2c` groups if your distro defines them.

---

## NVMe boot migration (recommended)

microSD works for development but slows cold-start, model load, and benchmark I/O. Before Wave 8 benchmarks, migrate rootfs to NVMe.

**Reference procedure:** [jetsonhacks/migrate-jetson-to-ssd](https://github.com/jetsonhacks/migrate-jetson-to-ssd) (adapt for Orin Nano Super carrier).

Summary:

1. Power off; install M.2 NVMe (2242/2280 per carrier spec).
2. Clone rootfs to NVMe (SDK Manager or `dd`/rsync scripts from jetsonhacks).
3. Configure UEFI/extlinux to boot from NVMe.
4. Confirm `lsblk` shows root on `nvme0n1p1` (or similar).
5. Document storage medium when publishing [BENCHMARKS.md](BENCHMARKS.md) rows (microSD vs NVMe).

---

## Active cooler and thermals

- Mount the **active cooler** included with the Super kit before running `./scripts/build_llama_jetson.sh` or sustained inference.
- Under load, monitor:

```bash
tegrastats --interval 1000
```

ShellClaw parses tegrastats for the Hardware Web UI GPU panel (`src/hardware/hardware_tegrastats.c`). See [LOCAL_INFERENCE.md](LOCAL_INFERENCE.md).

---

## Power modes (`nvpmodel`)

The Orin Nano **Super 8 GB** exposes **MAXN_SUPER** and **15W** modes. There is **no 7W mode** on this SKU (unlike some other Jetson modules).

```bash
sudo nvpmodel -q              # current mode
sudo nvpmodel -m 0            # example: MAXN (index varies by platform; use -q to list)
sudo nvpmodel -m 1            # 15W cap (verify index on your image)
```

For v1.0 sign-off:

- [ ] `nvpmodel -q` shows **MAXN_SUPER** selectable
- [ ] Run benchmarks in both MAXN_SUPER and 15W when publishing benchmarks

---

## GPIO and libgpiod

ShellClaw uses **libgpiod v2** with consumer name `shellclaw` (`src/hardware/hardware_libgpiod.c`).

### Detect chips

```bash
gpiodetect
```

Expected on JetPack 6.2.x Orin Nano:

| Device | Label |
|--------|-------|
| `/dev/gpiochip0` | `tegra234-gpio` — main 40-pin header GPIO |
| `/dev/gpiochip1` | `tegra234-gpio-aon` — always-on domain (not used for header tools in v1.0) |

Inspect lines:

```bash
gpioinfo gpiochip0
```

### Physical pin map

Authoritative mapping: `src/hardware/boards/jetson_orin_nano.h` (JetsonHacks J12 layout). Physical pin numbers are **1–40** on the expansion header.

| Pin | Function | gpiochip0 line | ShellClaw GPIO tool |
|-----|----------|----------------|---------------------|
| 7 | GPIO09 | 144 | Yes |
| 11 | UART1_RTS (SFIO) | 112 | **No** — SFIO |
| 13 | SPI1_SCK (SFIO) | 122 | **No** — SFIO |
| 15 | GPIO12 | 85 | Yes |
| 29 | GPIO01 | 105 | Yes |
| 31 | GPIO11 | 106 | Yes |
| 32 | GPIO07 | 41 | Yes |
| 33 | GPIO13 | 43 | Yes (default `gpio_test_pin`) |

Power/ground: pins 1, 4, 6, 9, 14, 17, 20, 25, 30, 34, 39 = 3V3/5V/GND — never driven as GPIO.

### Pinmux gotchas

Many header pins default to **SFIO** (I2C, UART, SPI, I2S). ShellClaw rejects SFIO pins:

```
pin N is configured as SFIO (I2C/UART/SPI) in pinmux
```

To use a pin as GPIO on Jetson you may need to:

1. Run **`jetson-io`** (JetPack) to reconfigure pinmux, **or**
2. Apply a device-tree overlay that sets the pin to GPIO mode.

Always cross-check with `gpioinfo` after pinmux changes. Re-verify line numbers after JetPack upgrades — pin tables can shift between releases.

### Release ritual (developer laptop)

Before tagging a release, validate libgpiod against a mock chip (no Jetson required):

```bash
sudo modprobe gpio-mockup gpio_mockup_ranges=-1,32
ls /dev/gpiochip*
SHELLCLAW_BOARD=jetson make test_hardware_libgpiod
sudo rmmod gpio-mockup
```

---

## I2C

| Item | Jetson Orin Nano Super |
|------|------------------------|
| Default bus | **7** (`/dev/i2c-7`) — configurable via `[hardware] i2c_bus` |
| Header I2C1 | Physical pins **3** (SDA), **5** (SCL) — SFIO, do not use GPIO tools on these |
| Scan tool | `i2c_scan(7)` returns JSON address list (empty array if no devices) |

Quick manual check:

```bash
sudo i2cdetect -y 7
```

ShellClaw opens `/dev/i2c-N` with `ioctl(I2C_SLAVE)`; address range 0x03–0x77.

### Sensor wiring (v1.2 preview)

**Not required for v1.0 sign-off.** Documented here for early hardware planning.

| Sensor | I2C address | Wiring (3V3 logic) |
|--------|-------------|-------------------|
| BME280 | 0x76 or 0x77 | VCC→3V3, GND→GND, SDA→pin 3, SCL→pin 5 |
| BH1750 | 0x23 or 0x5C | Same I2C bus as BME280 |

Pull-ups: module boards usually include them; bare chips need 3V3 pull-ups on SDA/SCL (typ. 4.7 kΩ).

**DHT22:** one-wire timing-sensitive; **not supported in v1.0**. Planned as **experimental** in v1.2 (Q-DHT22). Prefer I2C sensors for production sketches.

---

## Camera

### v1.0 vs v1.2

| Capability | v1.0 | v1.2 |
|------------|------|------|
| CSI capture CLI (`nvarguscamerasrc`) | Backend present; gateway rate-limited snapshot route | Full image return to LLM + Web UI |
| USB UVC (`v4l2-ctl`) | Backend present | Same |
| Multimodal chat (vision) | No | Phase 7 |

Web UI **Sensors** and **Camera** tabs show **“Coming in v1.2”** in v1.0.

### CSI (IMX219 / RPI camera module on Jetson adapter)

1. Power off; connect ribbon cable to CSI port (contacts face carrier board per NVIDIA silkscreen).
2. Verify Argus stack:

```bash
gst-launch-1.0 nvarguscamerasrc num-buffers=1 ! fakesink
```

ShellClaw spawns **fixed argv only** — `gst-launch-1.0` with `nvarguscamerasrc` (`src/hardware/hardware_camera.c`). No user-controlled pipeline strings.

**Security note:** `nvargus-daemon` runs as root; `/tmp/argus_socket` is **not** exposed to sandboxed shell commands. See [SECURITY.md](SECURITY.md).

### USB camera

Set in config:

```toml
[hardware.camera]
type = "usb"
resolution = "640x480"
```

Backend uses `v4l2-ctl` with validated path/resolution (no shell metacharacters).

---

## Install and on-device checklist

```bash
make shellclaw
./scripts/install.sh                    # systemd user units + llama env
./scripts/build_llama_jetson.sh         # CUDA llama-server → /usr/local/bin
./scripts/download_model.sh phi3        # Phi-3-mini Q4_K_M default
systemctl --user enable --now llama-server shellclaw
curl -sf http://localhost:18789/health
```

Functional v1.0 checks (from release-quality checklist):

- LLM calls `gpio_read(13)` end-to-end
- LLM calls `i2c_scan(7)` (empty array OK)
- Local provider active when cloud disabled (`/api/status`)
- `SHELLCLAW_HW_TEST=1 make test_hardware_on_device` exits 0

---

## Troubleshooting

| Symptom | Check |
|---------|-------|
| GPIO permission denied | User in `gpio` group; `/dev/gpiochip0` readable |
| SFIO error on pin | Use `jetson-io` or pick a GPIO-capable pin from pin table |
| I2C empty scan | Wiring, 3V3 level, `i2cdetect -y 7`, sensor not yet decoded in v1.0 |
| Camera spawn fails | `gst-launch-1.0` in PATH; CSI cable; Argus daemon running |
| Slow model load | Migrate to NVMe; see NVMe section |

---

## Related docs

- [HARDWARE_SAFETY.md](HARDWARE_SAFETY.md) — 3V3, current, ESD
- [LOCAL_INFERENCE.md](LOCAL_INFERENCE.md) — CUDA llama-server, memory budget
- [ARCHITECTURE.md](ARCHITECTURE.md) — software layout
