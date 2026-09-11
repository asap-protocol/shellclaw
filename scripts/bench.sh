#!/usr/bin/env bash
# ShellClaw performance benchmark harness (PRD §4.10, Phase 5 Wave 8).
#
# Wraps existing test binaries (test_sandbox clone benchmark) and collects
# Jetson-specific samples (tegrastats one-shot, nvpmodel power mode).
# On laptops / CI, hardware-only sections emit status=skip with a reason.
#
# Usage:
#   ./scripts/bench.sh                    # all sections, human-readable
#   ./scripts/bench.sh --json             # machine-readable JSON lines
#   ./scripts/bench.sh --section sandbox  # one section (repeatable)
#   BENCH_SET_POWER_MODE=1 ./scripts/bench.sh --power-mode MAXN_SUPER
#
# Environment:
#   SHELLCLAW_BIN              path to shellclaw (default: build/shellclaw)
#   BENCH_GATEWAY_URL          running gateway base URL (default: ephemeral boot)
#   BENCH_LLAMA_URL            llama-server OpenAI base (default: http://127.0.0.1:8080/v1)
#   BENCH_I2C_BUS              I2C bus number for scan latency (default: 7)
#   BENCH_STORAGE              force storage label: nvme | microsd
#   BENCH_SET_POWER_MODE=1     on Jetson, sudo nvpmodel before run (--power-mode)
#   BENCH_NVPMODEL_MAXN        nvpmodel mode id for MAXN_SUPER (default: 0)
#   BENCH_NVPMODEL_15W         nvpmodel mode id for 15W (default: 1)
#   BENCH_SKIP_BUILD=1         do not run make for missing test binaries
#   BENCH_WS_SAMPLES           HTTP /health RTT samples (default: 10)
#
# Jetson on-device ritual (fill docs/BENCHMARKS.md):
#   sudo nvpmodel -m 0 && sudo jetson_clocks   # MAXN_SUPER — verify with nvpmodel -q
#   ./scripts/bench.sh --power-mode MAXN_SUPER --storage nvme
#   sudo nvpmodel -m 1                           # 15W
#   ./scripts/bench.sh --power-mode 15W --storage nvme
#
# Note: Jetson Orin Nano Super 8 GB firmware has no 7 W mode (Brief §2).
#
# Exit codes: 0 completed (skips are OK), 1 usage/deps, 2 build failed, 3 gateway boot failed.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN="${SHELLCLAW_BIN:-${ROOT}/build/shellclaw}"
BINDIR="${BINDIR:-${ROOT}/build}"
JSON=0
SECTION="all"
POWER_MODE=""
STORAGE="${BENCH_STORAGE:-}"
WS_SAMPLES="${BENCH_WS_SAMPLES:-10}"
LLAMA_URL="${BENCH_LLAMA_URL:-http://127.0.0.1:8080/v1}"
I2C_BUS="${BENCH_I2C_BUS:-7}"

# Populated by detect_* helpers.
PLATFORM="unknown"
BOARD="unknown"
ARCH="$(uname -m 2>/dev/null || echo unknown)"
HOST="$(uname -s 2>/dev/null || echo unknown)"
JETSON=0

usage() {
	sed -n '2,35p' "$0" | sed 's/^# \{0,1\}//'
	exit 1
}

log() { printf '%s\n' "$*"; }
log_err() { printf '%s\n' "$*" >&2; }

now_ms() {
	local ms=""
	# BSD date accepts %3N but prints a literal "N"; require digits-only output.
	if ms="$(date +%s%3N 2>/dev/null)" && [[ "${ms}" =~ ^[0-9]+$ ]]; then
		printf '%s\n' "${ms}"
		return
	fi
	if command -v gdate >/dev/null 2>&1; then
		ms="$(gdate +%s%3N 2>/dev/null || true)"
		if [[ "${ms}" =~ ^[0-9]+$ ]]; then
			printf '%s\n' "${ms}"
			return
		fi
	fi
	if command -v python3 >/dev/null 2>&1; then
		python3 - <<'PY'
import time
print(int(time.time() * 1000))
PY
		return
	fi
	echo $(( $(date +%s) * 1000 ))
}

emit() {
	# emit key value [extra pairs...]
	local key="$1"
	local val="$2"
	shift 2
	if [[ "${JSON}" -eq 1 ]]; then
		local pairs
		pairs="\"${key}\":"
		if [[ "${val}" =~ ^[0-9]+(\.[0-9]+)?$ ]]; then
			pairs="${pairs}${val}"
		else
			pairs="${pairs}\"${val//\"/\\\"}\""
		fi
		while [[ $# -gt 0 ]]; do
			local ek="$1"
			local ev="$2"
			shift 2
			if [[ "${ev}" =~ ^[0-9]+(\.[0-9]+)?$ ]]; then
				pairs="${pairs}, \"${ek}\": ${ev}"
			else
				pairs="${pairs}, \"${ek}\": \"${ev//\"/\\\"}\""
			fi
		done
		printf '{%s}\n' "${pairs}"
	else
		if [[ $# -eq 0 ]]; then
			printf '%s=%s\n' "${key}" "${val}"
		else
			local extra=""
			while [[ $# -gt 0 ]]; do
				extra="${extra} $1=$2"
				shift 2
			done
			printf '%s=%s%s\n' "${key}" "${val}" "${extra}"
		fi
	fi
}

section_hdr() {
	local name="$1"
	if [[ "${JSON}" -eq 1 ]]; then
		emit "section" "${name}" "event" "begin"
	else
		printf '\n[%s]\n' "${name}"
	fi
}

detect_jetson() {
	if [[ -r /proc/device-tree/model ]]; then
		local model
		model="$(tr -d '\0' </proc/device-tree/model 2>/dev/null || true)"
		if [[ "${model}" == *"Jetson"* || "${model}" == *"NVIDIA"* ]]; then
			JETSON=1
			PLATFORM="jetson"
			BOARD="${model}"
			return 0
		fi
	fi
	if [[ -x "${BIN}" ]]; then
		local det
		det="$("${BIN}" --detect-board 2>/dev/null || true)"
		det="${det//$'\r'/}"
		det="${det%%$'\n'*}"
		if [[ "${det}" == jetson_* ]]; then
			JETSON=1
			PLATFORM="jetson"
			BOARD="${det}"
		fi
	fi
}

detect_platform() {
	if [[ "${HOST}" == "Linux" ]]; then
		detect_jetson
		if [[ "${JETSON}" -eq 0 ]]; then
			if [[ "${ARCH}" == "x86_64" || "${ARCH}" == "amd64" ]]; then
				PLATFORM="x86"
			else
				PLATFORM="linux"
			fi
		fi
	elif [[ "${HOST}" == "Darwin" ]]; then
		PLATFORM="macos"
	else
		PLATFORM="$(echo "${HOST}" | tr '[:upper:]' '[:lower:]')"
	fi
}

detect_storage() {
	if [[ -n "${STORAGE}" ]]; then
		return 0
	fi
	local src=""
	if command -v findmnt >/dev/null 2>&1; then
		src="$(findmnt -n -o SOURCE / 2>/dev/null || true)"
	elif [[ -r /proc/mounts ]]; then
		src="$(awk '$2=="/"{print $1; exit}' /proc/mounts 2>/dev/null || true)"
	fi
	case "${src}" in
	*nvme*) STORAGE="nvme" ;;
	*mmcblk*) STORAGE="microsd" ;;
	*) STORAGE="unknown" ;;
	esac
}

read_power_mode() {
	if ! command -v nvpmodel >/dev/null 2>&1; then
		printf '%s' "unknown"
		return 0
	fi
	local line mode
	line="$(nvpmodel -q 2>/dev/null | grep -F 'NV Power Mode:' | head -n1 || true)"
	mode="${line#*NV Power Mode:}"
	mode="${mode#"${mode%%[![:space:]]*}"}"
	if [[ -z "${mode}" ]]; then
		printf '%s' "unknown"
	else
		printf '%s' "${mode}"
	fi
}

maybe_set_power_mode() {
	local want="$1"
	if [[ -z "${want}" || "${JETSON}" -eq 0 ]]; then
		return 0
	fi
	if [[ "${BENCH_SET_POWER_MODE:-}" != "1" ]]; then
		emit "power_mode_note" "read-only" "requested" "${want}" "hint" "set BENCH_SET_POWER_MODE=1 to sudo nvpmodel"
		return 0
	fi
	local mode_id=""
	case "${want}" in
	MAXN_SUPER | MAXN | maxn*)
		mode_id="${BENCH_NVPMODEL_MAXN:-0}"
		;;
	15W | 15w | 15*)
		mode_id="${BENCH_NVPMODEL_15W:-1}"
		;;
	*)
		log_err "bench: unknown --power-mode ${want} (use MAXN_SUPER or 15W)"
		exit 1
		;;
	esac
	if ! command -v nvpmodel >/dev/null 2>&1; then
		emit "power_mode_set" "skip" "reason" "nvpmodel_missing"
		return 0
	fi
	log "==> sudo nvpmodel -m ${mode_id} (${want})"
	sudo nvpmodel -m "${mode_id}"
	if command -v jetson_clocks >/dev/null 2>&1 && [[ "${want}" == MAXN* ]]; then
		sudo jetson_clocks || true
	fi
	sleep 2
}

ensure_test_sandbox() {
	local exe="${BINDIR}/test_sandbox"
	if [[ -x "${exe}" ]]; then
		return 0
	fi
	if [[ "${BENCH_SKIP_BUILD:-}" == "1" ]]; then
		return 1
	fi
	( cd "${ROOT}" && make test_sandbox >/dev/null )
	[[ -x "${exe}" ]]
}

proc_rss_kb() {
	local pid="$1"
	if [[ ! -r "/proc/${pid}/status" ]]; then
		return 1
	fi
	awk '/^VmRSS:/ { print $2; exit }' "/proc/${pid}/status"
}

bench_meta() {
	section_hdr "meta"
	local pm
	pm="$(read_power_mode)"
	if [[ -n "${POWER_MODE}" ]]; then
		pm="${POWER_MODE}"
	fi
	emit "timestamp" "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
	emit "platform" "${PLATFORM}"
	emit "board" "${BOARD}"
	emit "arch" "${ARCH}"
	emit "storage" "${STORAGE}"
	emit "power_mode" "${pm}"
	emit "jetson" "${JETSON}"
	emit "shellclaw_bin" "${BIN}"
}

bench_tegrastats() {
	section_hdr "tegrastats"
	if [[ "${JETSON}" -eq 0 ]]; then
		emit "tegrastats" "skip" "reason" "not_jetson"
		return 0
	fi
	if ! command -v tegrastats >/dev/null 2>&1; then
		emit "tegrastats" "skip" "reason" "tegrastats_missing"
		return 0
	fi
	local line err
	line="$(tegrastats --interval 100 --count 1 2>/dev/null | head -n1 || true)"
	if [[ -z "${line}" ]]; then
		emit "tegrastats" "skip" "reason" "no_output"
		return 0
	fi
	emit "tegrastats_line" "${line}" "status" "ok"
	# Parse RAM used/total and GR3D via test_hardware_tegrastats logic (Python one-liner).
	python3 - "${line}" <<'PY'
import re, sys
line = sys.argv[1]
ram = re.search(r"RAM (\d+)/(\d+)MB", line)
gr3d = re.search(r"GR3D_FREQ (\d+)%@\[(\d+),(\d+)\]", line) or re.search(r"GR3D_FREQ (\d+)%@(\d+)", line)
gpu_temp = re.search(r"gpu@([\d.]+)C", line)
out = []
if ram:
    out += [("ram_used_mb", int(ram.group(1))), ("ram_total_mb", int(ram.group(2)))]
if gr3d:
    out += [("gpu_usage_pct", int(gr3d.group(1)))]
    freq = max(int(gr3d.group(2)), int(gr3d.group(3))) if gr3d.lastindex and gr3d.lastindex >= 3 else int(gr3d.group(2))
    out += [("gpu_freq_mhz", freq)]
if gpu_temp:
    out += [("gpu_temp_c", float(gpu_temp.group(1)))]
for k, v in out:
    print(f"{k}={v}")
PY
	while IFS='=' read -r k v; do
		[[ -n "${k}" ]] && emit "${k}" "${v}" "status" "ok"
	done
}

bench_sandbox() {
	section_hdr "sandbox"
	if ! ensure_test_sandbox; then
		emit "sandbox_median_us" "skip" "reason" "test_sandbox_missing"
		return 0
	fi
	local out median avg
	out="$("${BINDIR}/test_sandbox" 2>&1 || true)"
	median="$(printf '%s\n' "${out}" | sed -n "s/.*median=\\([0-9]*\\) µs.*/\\1/p" | head -n1)"
	avg="$(printf '%s\n' "${out}" | sed -n "s/.*avg=\\([0-9]*\\) µs.*/\\1/p" | head -n1)"
	if [[ -z "${median}" ]]; then
		emit "sandbox_median_us" "skip" "reason" "parse_failed"
		return 0
	fi
	emit "sandbox_median_us" "${median}" "sandbox_avg_us" "${avg:-unknown}" "status" "ok"
	emit "sandbox_target_us" "1000" "note" "PRD clone+setup target under 1 ms"
}

start_ephemeral_gateway() {
	local host port cfg tmp_home started_pid health_url
	host="127.0.0.1"
	port="$(python3 - <<'PY'
import socket
s = socket.socket()
s.bind(("127.0.0.1", 0))
print(s.getsockname()[1])
s.close()
PY
)"
	tmp_home="$(mktemp -d)"
	export SHELLCLAW_HOME="${tmp_home}/.shellclaw"
	mkdir -p "${SHELLCLAW_HOME}/keys" "${SHELLCLAW_HOME}/skills"
	cfg="${SHELLCLAW_HOME}/config.toml"
	cat >"${cfg}" <<EOF
[agent]
model = "bench-stub"

[providers]
fallback_chain = [ "stub" ]

[gateway]
enabled = true
host = "${host}"
port = ${port}
require_pairing = false

[memory]
db_path = "${SHELLCLAW_HOME}/memory.db"

[skills]
dir = "${SHELLCLAW_HOME}/skills"

[hardware]
enabled = true
EOF
	health_url="http://${host}:${port}/health"
	SHELLCLAW_BOARD="${SHELLCLAW_BOARD:-jetson}" SHELLCLAW_TEST_MODE=1 \
		"${BIN}" --config "${cfg}" >>"${SHELLCLAW_HOME}/shellclaw.log" 2>&1 &
	started_pid=$!
	local deadline=$((SECONDS + 60))
	while ((SECONDS < deadline)); do
		local code
		code="$(curl -sS -o /dev/null -w '%{http_code}' --connect-timeout 1 --max-time 2 "${health_url}" 2>/dev/null || echo 000)"
		if [[ "${code}" == "200" ]]; then
			printf '%s\n' "${started_pid}" "${health_url}" "${cfg}" "${tmp_home}"
			return 0
		fi
		if ! kill -0 "${started_pid}" 2>/dev/null; then
			break
		fi
		sleep 0.2
	done
	kill "${started_pid}" 2>/dev/null || true
	wait "${started_pid}" 2>/dev/null || true
	rm -rf "${tmp_home}"
	return 1
}

stop_ephemeral_gateway() {
	local pid="$1"
	local tmp_home="$2"
	if [[ -n "${pid}" ]] && kill -0 "${pid}" 2>/dev/null; then
		kill "${pid}" 2>/dev/null || true
		wait "${pid}" 2>/dev/null || true
	fi
	if [[ -n "${tmp_home}" && -d "${tmp_home}" ]]; then
		chmod -R u+w "${tmp_home}" 2>/dev/null || true
		rm -rf "${tmp_home}"
	fi
}

bench_cold_start() {
	section_hdr "cold_start"
	if [[ ! -x "${BIN}" ]]; then
		emit "cold_start_ms" "skip" "reason" "shellclaw_missing"
		return 0
	fi
	local t0 t1 info pid health_url tmp_home cold_ms
	t0="$(now_ms)"
	if ! info="$(start_ephemeral_gateway)"; then
		emit "cold_start_ms" "skip" "reason" "gateway_boot_failed"
		return 0
	fi
	pid="$(printf '%s\n' "${info}" | sed -n '1p')"
	health_url="$(printf '%s\n' "${info}" | sed -n '2p')"
	tmp_home="$(printf '%s\n' "${info}" | sed -n '4p')"
	t1="$(now_ms)"
	cold_ms=$((t1 - t0))
	emit "cold_start_ms" "${cold_ms}" "status" "ok" "note" "gateway /health 200, stub provider"
	stop_ephemeral_gateway "${pid}" "${tmp_home}"
}

bench_ram() {
	section_hdr "ram"
	if [[ ! -x "${BIN}" ]]; then
		emit "idle_rss_kb" "skip" "reason" "shellclaw_missing"
		return 0
	fi
	local info pid health_url tmp_home idle_kb
	if ! info="$(start_ephemeral_gateway)"; then
		emit "idle_rss_kb" "skip" "reason" "gateway_boot_failed"
		return 0
	fi
	pid="$(printf '%s\n' "${info}" | sed -n '1p')"
	tmp_home="$(printf '%s\n' "${info}" | sed -n '4p')"
	sleep 1
	if idle_kb="$(proc_rss_kb "${pid}" 2>/dev/null)"; then
		emit "idle_rss_kb" "${idle_kb}" "status" "ok" "note" "agent process only; llama-server excluded"
	else
		# macOS fallback
		idle_kb="$(ps -o rss= -p "${pid}" 2>/dev/null | tr -d ' ' || echo "")"
		if [[ -n "${idle_kb}" ]]; then
			emit "idle_rss_kb" "${idle_kb}" "status" "ok" "note" "ps RSS pages/kb platform-dependent"
		else
			emit "idle_rss_kb" "skip" "reason" "rss_unavailable"
		fi
	fi
	# Active RAM: trigger stub chat via HTTP status poll (minimal agent work).
	health_url="$(printf '%s\n' "${info}" | sed -n '2p')"
	curl -sS -o /dev/null "${health_url%/health}/api/status" 2>/dev/null || true
	sleep 0.5
	local active_kb
	if active_kb="$(proc_rss_kb "${pid}" 2>/dev/null)"; then
		emit "active_rss_kb" "${active_kb}" "status" "ok" "note" "after /api/status; not a full LLM call"
	else
		active_kb="$(ps -o rss= -p "${pid}" 2>/dev/null | tr -d ' ' || echo "")"
		if [[ -n "${active_kb}" ]]; then
			emit "active_rss_kb" "${active_kb}" "status" "ok"
		else
			emit "active_rss_kb" "skip" "reason" "rss_unavailable"
		fi
	fi
	stop_ephemeral_gateway "${pid}" "${tmp_home}"
}

bench_i2c() {
	section_hdr "i2c"
	if [[ "${JETSON}" -eq 0 && ! -e "/dev/i2c-${I2C_BUS}" ]]; then
		emit "i2c_scan_ms" "skip" "reason" "no_i2c_bus" "hint" "run on Jetson with /dev/i2c-${I2C_BUS}"
		return 0
	fi
	if ! command -v i2cdetect >/dev/null 2>&1; then
		emit "i2c_scan_ms" "skip" "reason" "i2cdetect_missing"
		return 0
	fi
	local t0 t1 ms
	t0="$(now_ms)"
	i2cdetect -y "${I2C_BUS}" >/dev/null 2>&1 || {
		emit "i2c_scan_ms" "skip" "reason" "i2cdetect_failed" "bus" "${I2C_BUS}"
		return 0
	}
	t1="$(now_ms)"
	ms=$((t1 - t0))
	emit "i2c_scan_ms" "${ms}" "bus" "${I2C_BUS}" "status" "ok"
}

bench_camera() {
	section_hdr "camera"
	if [[ "${JETSON}" -eq 0 ]]; then
		emit "camera_cold_ms" "skip" "reason" "not_jetson"
		emit "camera_warm_ms" "skip" "reason" "not_jetson"
		return 0
	fi
	if ! command -v gst-launch-1.0 >/dev/null 2>&1; then
		emit "camera_cold_ms" "skip" "reason" "gstreamer_missing"
		emit "camera_warm_ms" "skip" "reason" "gstreamer_missing"
		return 0
	fi
	local out="/tmp/shellclaw_bench_cam.jpg"
	local t0 t1 cold warm
	rm -f "${out}"
	t0="$(now_ms)"
	if ! timeout 30 gst-launch-1.0 -e nvarguscamerasrc num-buffers=1 ! jpegenc ! filesink location="${out}" >/dev/null 2>&1; then
		emit "camera_cold_ms" "skip" "reason" "capture_failed" "hint" "CSI camera required"
		emit "camera_warm_ms" "skip" "reason" "capture_failed"
		return 0
	fi
	t1="$(now_ms)"
	cold=$((t1 - t0))
	emit "camera_cold_ms" "${cold}" "status" "ok"
	t0="$(now_ms)"
	timeout 30 gst-launch-1.0 -e nvarguscamerasrc num-buffers=1 ! jpegenc ! filesink location="${out}" >/dev/null 2>&1 || true
	t1="$(now_ms)"
	warm=$((t1 - t0))
	emit "camera_warm_ms" "${warm}" "status" "ok"
	rm -f "${out}"
}

bench_ws_rtt() {
	section_hdr "websocket"
	if ! command -v curl >/dev/null 2>&1; then
		emit "gateway_http_rtt_ms" "skip" "reason" "curl_missing"
		return 0
	fi
	local base="${BENCH_GATEWAY_URL:-}"
	local pid="" tmp_home=""
	if [[ -z "${base}" ]]; then
		local info
		if ! info="$(start_ephemeral_gateway)"; then
			emit "gateway_http_rtt_ms" "skip" "reason" "gateway_boot_failed"
			return 0
		fi
		pid="$(printf '%s\n' "${info}" | sed -n '1p')"
		base="$(printf '%s\n' "${info}" | sed -n '2p')"
		base="${base%/health}"
		tmp_home="$(printf '%s\n' "${info}" | sed -n '4p')"
	fi
	local samples=() i code t0 t1 ms sum=0
	for ((i = 0; i < WS_SAMPLES; i++)); do
		t0="$(now_ms)"
		code="$(curl -sS -o /dev/null -w '%{http_code}' --connect-timeout 2 --max-time 5 "${base}/health" 2>/dev/null || echo 000)"
		t1="$(now_ms)"
		if [[ "${code}" != "200" ]]; then
			break
		fi
		ms=$((t1 - t0))
		samples+=("${ms}")
		sum=$((sum + ms))
	done
	if [[ ${#samples[@]} -eq 0 ]]; then
		emit "gateway_http_rtt_ms" "skip" "reason" "health_failed"
	else
		IFS=$'\n' sorted=($(printf '%s\n' "${samples[@]}" | sort -n))
		local median="${sorted[$(( ${#sorted[@]} / 2 ))]}"
		local avg=$((sum / ${#samples[@]}))
		emit "gateway_http_rtt_median_ms" "${median}" "gateway_http_rtt_avg_ms" "${avg}" "samples" "${#samples[@]}" "status" "ok"
		emit "ws_note" "HTTP health RTT proxy; full WS chat RTT overlaps agent_loop with paired gateway"
	fi
	[[ -n "${pid}" ]] && stop_ephemeral_gateway "${pid}" "${tmp_home}"
}

llama_reachable() {
	local code
	code="$(curl -sS -o /dev/null -w '%{http_code}' --connect-timeout 2 --max-time 5 \
		"${LLAMA_URL}/models" 2>/dev/null || echo 000)"
	[[ "${code}" == "200" ]]
}

bench_llm() {
	section_hdr "llm"
	if ! llama_reachable; then
		emit "llm_gen_tok_s" "skip" "reason" "llama_server_unreachable" "url" "${LLAMA_URL}"
		emit "llm_prefill_tok_s" "skip" "reason" "llama_server_unreachable"
		emit "llm_gpu_ram_mb" "skip" "reason" "llama_server_unreachable"
		return 0
	fi
	local model prompt t0 t1 elapsed_ms completion_tokens gen_tok_s
	model="${BENCH_LLM_MODEL:-Phi-3-mini-4k-instruct-Q4_K_M}"
	prompt="Benchmark prompt for ShellClaw Wave 8. Reply with one short sentence."
	t0="$(now_ms)"
	local body_file http_code
	body_file="$(mktemp)"
	http_code="$(curl -sS -o "${body_file}" -w '%{http_code}' --connect-timeout 5 --max-time 120 \
		-H 'Content-Type: application/json' \
		-d "{\"model\":\"${model}\",\"messages\":[{\"role\":\"user\",\"content\":\"${prompt}\"}],\"max_tokens\":64,\"stream\":false}" \
		"${LLAMA_URL}/chat/completions" 2>/dev/null || echo 000)"
	t1="$(now_ms)"
	elapsed_ms=$((t1 - t0))
	if [[ "${http_code}" != "200" ]]; then
		rm -f "${body_file}"
		emit "llm_gen_tok_s" "skip" "reason" "completion_failed" "http" "${http_code}"
		return 0
	fi
	completion_tokens="$(python3 - "${body_file}" <<'PY'
import json, sys
try:
    d = json.load(open(sys.argv[1]))
    u = d.get("usage") or {}
    print(int(u.get("completion_tokens") or 0))
except Exception:
    print(0)
PY
)"
	rm -f "${body_file}"
	if [[ "${completion_tokens}" -gt 0 && "${elapsed_ms}" -gt 0 ]]; then
		gen_tok_s="$(python3 - <<PY
ct = ${completion_tokens}
ms = ${elapsed_ms}
print(f"{ct * 1000.0 / ms:.1f}")
PY
)"
		emit "llm_gen_tok_s" "${gen_tok_s}" "completion_tokens" "${completion_tokens}" "elapsed_ms" "${elapsed_ms}" "status" "ok"
	else
		emit "llm_gen_tok_s" "skip" "reason" "no_token_usage"
	fi
	# Prefill: longer context prompt (approximate — server may truncate).
	local long_prompt prefill_tok_s
	long_prompt="$(python3 - <<'PY'
print("Summarize edge AI. " * 400)
PY
)"
	t0="$(now_ms)"
	body_file="$(mktemp)"
	http_code="$(curl -sS -o "${body_file}" -w '%{http_code}' --connect-timeout 5 --max-time 120 \
		-H 'Content-Type: application/json' \
		-d "{\"model\":\"${model}\",\"messages\":[{\"role\":\"user\",\"content\":$(python3 -c 'import json,sys; print(json.dumps(sys.stdin.read()))' <<<"${long_prompt}")}],\"max_tokens\":1,\"stream\":false}" \
		"${LLAMA_URL}/chat/completions" 2>/dev/null || echo 000)"
	t1="$(now_ms)"
	elapsed_ms=$((t1 - t0))
	if [[ "${http_code}" == "200" ]]; then
		local prompt_tokens
		prompt_tokens="$(python3 - "${body_file}" <<'PY'
import json, sys
try:
    d = json.load(open(sys.argv[1]))
    u = d.get("usage") or {}
    print(int(u.get("prompt_tokens") or 0))
except Exception:
    print(0)
PY
)"
		if [[ "${prompt_tokens}" -gt 0 && "${elapsed_ms}" -gt 0 ]]; then
			prefill_tok_s="$(python3 - <<PY
pt = ${prompt_tokens}
ms = ${elapsed_ms}
print(f"{pt * 1000.0 / ms:.1f}")
PY
)"
			emit "llm_prefill_tok_s" "${prefill_tok_s}" "prompt_tokens" "${prompt_tokens}" "status" "ok"
		fi
	fi
	rm -f "${body_file}"
	if [[ "${JETSON}" -eq 1 ]] && command -v tegrastats >/dev/null 2>&1; then
		local line used total
		line="$(tegrastats --interval 100 --count 1 2>/dev/null | head -n1 || true)"
		if [[ "${line}" =~ RAM\ ([0-9]+)/([0-9]+)MB ]]; then
			used="${BASH_REMATCH[1]}"
			total="${BASH_REMATCH[2]}"
			emit "llm_unified_ram_used_mb" "${used}" "llm_unified_ram_total_mb" "${total}" "status" "ok"
		fi
	fi
}

bench_agent_loop() {
	section_hdr "agent_loop"
	if [[ ! -x "${BIN}" ]]; then
		emit "agent_loop_ms" "skip" "reason" "shellclaw_missing"
		return 0
	fi
	local tmp_home cfg t0 t1 ms
	tmp_home="$(mktemp -d)"
	export SHELLCLAW_HOME="${tmp_home}/.shellclaw"
	mkdir -p "${SHELLCLAW_HOME}/skills"
	cfg="${SHELLCLAW_HOME}/config.toml"
	cat >"${cfg}" <<EOF
[agent]
model = "bench-stub"

[providers]
fallback_chain = [ "stub" ]

[memory]
db_path = "${SHELLCLAW_HOME}/memory.db"

[skills]
dir = "${SHELLCLAW_HOME}/skills"
EOF
	t0="$(now_ms)"
	SHELLCLAW_BOARD=stub SHELLCLAW_TEST_MODE=1 \
		"${BIN}" --config "${cfg}" -m "bench ping" >/dev/null 2>&1 || true
	t1="$(now_ms)"
	ms=$((t1 - t0))
	emit "agent_loop_ms" "${ms}" "provider" "stub" "status" "ok" "note" "one-shot -m; use local provider on Jetson for LLM e2e"
	chmod -R u+w "${tmp_home}" 2>/dev/null || true
	rm -rf "${tmp_home}"
}

run_section() {
	case "$1" in
	all)
		bench_meta
		bench_tegrastats
		bench_cold_start
		bench_ram
		bench_sandbox
		bench_i2c
		bench_camera
		bench_ws_rtt
		bench_llm
		bench_agent_loop
		;;
	meta) bench_meta ;;
	tegrastats) bench_tegrastats ;;
	cold_start) bench_cold_start ;;
	ram) bench_ram ;;
	sandbox) bench_sandbox ;;
	i2c) bench_i2c ;;
	camera) bench_camera ;;
	websocket | ws) bench_ws_rtt ;;
	llm) bench_llm ;;
	agent_loop) bench_agent_loop ;;
	*)
		log_err "bench: unknown section: $1"
		exit 1
		;;
	esac
}

while [[ $# -gt 0 ]]; do
	case "$1" in
	-h | --help) usage ;;
	--json) JSON=1; shift ;;
	--section)
		shift
		[[ $# -ge 1 ]] || usage
		SECTION="$1"
		shift
		;;
	--power-mode)
		shift
		[[ $# -ge 1 ]] || usage
		POWER_MODE="$1"
		shift
		;;
	--storage)
		shift
		[[ $# -ge 1 ]] || usage
		STORAGE="$1"
		shift
		;;
	*) log_err "bench: unknown argument: $1"; usage ;;
	esac
done

if [[ ! -x "${BIN}" && "${BENCH_SKIP_BUILD:-}" != "1" ]]; then
	log "==> building shellclaw (${BIN})"
	( cd "${ROOT}" && make shellclaw >/dev/null ) || exit 2
fi

detect_platform
detect_storage
maybe_set_power_mode "${POWER_MODE}"

if [[ "${JSON}" -eq 0 ]]; then
	log "==> ShellClaw benchmark (platform=${PLATFORM} storage=${STORAGE})"
fi

run_section "${SECTION}"

if [[ "${JSON}" -eq 0 ]]; then
	log ""
	log "==> bench complete — paste key=value lines into docs/BENCHMARKS.md on Jetson"
fi
