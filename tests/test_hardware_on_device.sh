#!/usr/bin/env bash
# On-device hardware validation (Jetson Orin Nano Super, v1.0 scope).
#
# Gate: set SHELLCLAW_HW_TEST=1 on real hardware. Without it, exits 0 (CI-safe).
# Wrong host with SHELLCLAW_HW_TEST=1 exits 77 (EX_NOPERM) so operators notice misconfiguration.
#
# Checks: board detect, GPIO on gpio_test_pin, I2C scan (empty bus OK), llama-server HTTP smoke.
# Out of scope v1.0: sensors, camera (v1.2).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN="${SHELLCLAW_TEST_BIN:-${ROOT}/build/shellclaw}"
LLAMA_URL="${SHELLCLAW_LLAMA_URL:-http://127.0.0.1:8080/v1}"
LLAMA_MODEL="${SHELLCLAW_LLAMA_MODEL:-Phi-3-mini-4k-instruct-Q4_K_M}"
CONFIG="${SHELLCLAW_CONFIG:-${HOME}/.shellclaw/config.toml}"
FIXED_PROMPT="ShellClaw on-device hardware test. Reply with OK."

skip_ok() {
	printf 'test_hardware_on_device: %s\n' "$1"
	exit 0
}

fail() {
	printf 'test_hardware_on_device: FAIL — %s\n' "$1" >&2
	exit 1
}

if [[ "${SHELLCLAW_HW_TEST:-}" != "1" ]]; then
	skip_ok "skip (set SHELLCLAW_HW_TEST=1 on Jetson to run GPIO/I2C/llama checks)"
fi

if [[ ! -x "${BIN}" ]]; then
	fail "build shellclaw first (${BIN})"
fi

board="$("${BIN}" --detect-board 2>/dev/null || true)"
if [[ "${board}" != "jetson_orin_nano" ]]; then
	printf 'test_hardware_on_device: skip — SHELLCLAW_HW_TEST=1 but board=%s (need jetson_orin_nano)\n' \
		"${board:-unknown}" >&2
	exit 77
fi

read_config_int() {
	local key="$1" default="$2"
	if [[ ! -f "${CONFIG}" ]]; then
		printf '%s' "${default}"
		return 0
	fi
	python3 - "${CONFIG}" "${key}" "${default}" <<'PY'
import re, sys
path, key, default = sys.argv[1], sys.argv[2], int(sys.argv[3])
try:
    text = open(path, encoding="utf-8").read()
except OSError:
    print(default)
    raise SystemExit
m = re.search(r'(?m)^\s*' + re.escape(key) + r'\s*=\s*(\d+)', text)
print(int(m.group(1)) if m else default)
PY
}

jetson_line_for_hdr_pin() {
	local hdr_pin="$1"
	python3 - "${hdr_pin}" <<'PY'
# Physical header pin -> gpiochip0 line (Jetson Orin Nano J12, see jetson_orin_nano.h)
import sys
pin = int(sys.argv[1])
table = {
    7: 144, 15: 85, 29: 105, 31: 106, 32: 41, 33: 43,
}
line = table.get(pin)
if line is None:
    raise SystemExit(f"no gpiochip0 line mapping for header pin {pin}")
print(line)
PY
}

test_gpio_pin() {
	local hdr_pin line chip
	if ! command -v gpioget >/dev/null 2>&1; then
		fail "gpioget not found (install libgpiod tools)"
	fi
	hdr_pin="${SHELLCLAW_GPIO_TEST_PIN:-$(read_config_int gpio_test_pin 33)}"
	line="$(jetson_line_for_hdr_pin "${hdr_pin}")" || fail "unsupported gpio_test_pin=${hdr_pin}"
	chip="gpiochip0"
	if [[ ! -e "/dev/${chip}" ]]; then
		fail "/dev/${chip} missing (gpiodetect?)"
	fi
	gpioget "${chip}" "${line}" >/dev/null || fail "gpioget ${chip} ${line} (header pin ${hdr_pin})"
	if command -v gpioset >/dev/null 2>&1; then
		local before after
		before="$(gpioget "${chip}" "${line}" | awk '{print $NF}')"
		gpioset --mode=exit "${chip}" "${line}=$((1 - before))" >/dev/null 2>&1 \
			|| gpioset "${chip}" "${line}=$((1 - before))" >/dev/null \
			|| fail "gpioset toggle on ${chip} line ${line}"
		after="$(gpioget "${chip}" "${line}" | awk '{print $NF}')"
		if [[ "${before}" == "${after}" ]]; then
			fail "GPIO line ${line} did not change after gpioset (pin ${hdr_pin})"
		fi
		gpioset --mode=exit "${chip}" "${line}=${before}" >/dev/null 2>&1 \
			|| gpioset "${chip}" "${line}=${before}" >/dev/null 2>&1 \
			|| true
	fi
	printf 'test_hardware_on_device: GPIO header pin %s (line %s) OK\n' "${hdr_pin}" "${line}"
}

test_i2c_scan() {
	local bus
	bus="${SHELLCLAW_I2C_BUS:-$(read_config_int i2c_bus 7)}"
	if command -v i2cdetect >/dev/null 2>&1; then
		if [[ ! -e "/dev/i2c-${bus}" ]]; then
			fail "/dev/i2c-${bus} missing"
		fi
		i2cdetect -y "${bus}" >/dev/null || fail "i2cdetect -y ${bus}"
	elif [[ -x "${ROOT}/build/test_hardware_i2c" ]]; then
		"${ROOT}/build/test_hardware_i2c" >/dev/null || fail "test_hardware_i2c binary"
	else
		fail "i2cdetect not found and build/test_hardware_i2c missing"
	fi
	printf 'test_hardware_on_device: I2C bus %s scan OK (empty bus allowed)\n' "${bus}"
}

test_llama_smoke() {
	local code body_file http_code completion
	code="$(curl -sS -o /dev/null -w '%{http_code}' --connect-timeout 2 --max-time 5 \
		"${LLAMA_URL}/models" 2>/dev/null || echo 000)"
	if [[ "${code}" != "200" ]]; then
		fail "llama-server unreachable at ${LLAMA_URL} (HTTP ${code})"
	fi
	body_file="$(mktemp)"
	trap 'rm -f "${body_file}"' RETURN
	http_code="$(curl -sS -o "${body_file}" -w '%{http_code}' --connect-timeout 5 --max-time 120 \
		-H 'Content-Type: application/json' \
		-d "{\"model\":\"${LLAMA_MODEL}\",\"messages\":[{\"role\":\"user\",\"content\":$(python3 -c 'import json,sys; print(json.dumps(sys.argv[1]))' "${FIXED_PROMPT}")}],\"max_tokens\":32,\"stream\":false}" \
		"${LLAMA_URL}/chat/completions" 2>/dev/null || echo 000)"
	if [[ "${http_code}" != "200" ]]; then
		fail "llama chat/completions HTTP ${http_code}"
	fi
	completion="$(python3 - "${body_file}" <<'PY'
import json, sys
try:
    d = json.load(open(sys.argv[1]))
    ch = (d.get("choices") or [{}])[0]
    msg = ch.get("message") or {}
    print((msg.get("content") or "").strip())
except Exception:
    print("")
PY
)"
	if [[ -z "${completion}" ]]; then
		fail "llama completion empty"
	fi
	printf 'test_hardware_on_device: llama-server smoke OK (%d chars)\n' "${#completion}"
}

test_gpio_pin
test_i2c_scan
test_llama_smoke
printf 'test_hardware_on_device: all checks passed\n'
