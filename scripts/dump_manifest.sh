#!/usr/bin/env bash
# Dump the live gateway SignedManifest (GET /.well-known/asap/manifest.json).
#
# Boots a short-lived shellclaw process on 127.0.0.1 unless MANIFEST_DUMP_SKIP_START=1
# and the gateway already answers /health. Headless and CI-friendly (curl, timeouts,
# isolated SHELLCLAW_HOME with generated Ed25519 keys on first serve).
#
# Usage:
#   ./scripts/dump_manifest.sh              # write JSON to stdout
#   ./scripts/dump_manifest.sh -o FILE      # write JSON to FILE
#   ./scripts/dump_manifest.sh --stdout     # explicit stdout (default)
#
# Environment:
#   SHELLCLAW_BIN          path to shellclaw binary (default: build/shellclaw)
#   SHELLCLAW_HOME         state dir (default: temp dir under $TMPDIR)
#   SHELLCLAW_BOARD        board profile for manifest (default: jetson)
#   SHELLCLAW_GATEWAY_HOST gateway bind host (default: 127.0.0.1)
#   SHELLCLAW_GATEWAY_PORT gateway port (default: 18789, matches config.example.toml)
#   SHELLCLAW_ASAP_PUBLIC_BASE_URL  [asap] public_base_url in generated config
#   MANIFEST_DUMP_TIMEOUT_SEC     health poll timeout (default: 60)
#   MANIFEST_DUMP_SKIP_START=1    skip boot; require gateway already listening
#   MANIFEST_DUMP_PRE_ROTATE=1    run --rotate-keys before start (optional fresh keys)
#
# Exit codes: 0 success, 1 usage/validation, 2 missing binary, 3 boot/health timeout,
#             4 manifest fetch failed.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN="${SHELLCLAW_BIN:-${ROOT}/build/shellclaw}"
HOST="${SHELLCLAW_GATEWAY_HOST:-127.0.0.1}"
PORT="${SHELLCLAW_GATEWAY_PORT:-18789}"
BOARD="${SHELLCLAW_BOARD:-jetson}"
PUBLIC_BASE="${SHELLCLAW_ASAP_PUBLIC_BASE_URL:-https://asap-protocol.github.io/shellclaw}"
TIMEOUT_SEC="${MANIFEST_DUMP_TIMEOUT_SEC:-60}"
OUT_FILE=""
STARTED_PID=""

usage() {
	sed -n '2,22p' "$0" | sed 's/^# \{0,1\}//'
	exit 1
}

cleanup() {
	if [[ -n "${STARTED_PID}" ]] && kill -0 "${STARTED_PID}" 2>/dev/null; then
		kill "${STARTED_PID}" 2>/dev/null || true
		wait "${STARTED_PID}" 2>/dev/null || true
	fi
	if [[ -n "${TMP_HOME:-}" && -d "${TMP_HOME}" ]]; then
		chmod -R u+w "${TMP_HOME}" >/dev/null 2>&1 || true
		rm -rf "${TMP_HOME}"
	fi
}
trap cleanup EXIT

while [[ $# -gt 0 ]]; do
	case "$1" in
	-h | --help) usage ;;
	-o | --output)
		shift
		[[ $# -ge 1 ]] || usage
		OUT_FILE="$1"
		shift
		;;
	--stdout)
		OUT_FILE=""
		shift
		;;
	*) echo "dump_manifest: unknown argument: $1" >&2; usage ;;
	esac
done

if [[ ! -x "${BIN}" ]]; then
	echo "dump_manifest: build shellclaw first (${BIN})" >&2
	exit 2
fi

if ! command -v curl >/dev/null 2>&1; then
	echo "dump_manifest: curl required" >&2
	exit 1
fi

if [[ -z "${SHELLCLAW_HOME:-}" ]]; then
	TMP_HOME="$(mktemp -d)"
	export SHELLCLAW_HOME="${TMP_HOME}/.shellclaw"
else
	TMP_HOME=""
	export SHELLCLAW_HOME
fi
mkdir -p "${SHELLCLAW_HOME}/keys" "${SHELLCLAW_HOME}/skills"

CFG="${SHELLCLAW_HOME}/config.toml"
cat >"${CFG}" <<EOF
[agent]
model = "manifest-dump"

[providers]
fallback_chain = [ "stub" ]

[gateway]
enabled = true
host = "${HOST}"
port = ${PORT}

[memory]
db_path = "${SHELLCLAW_HOME}/memory.db"

[skills]
dir = "${SHELLCLAW_HOME}/skills"

[asap]
agent_urn = "urn:asap:agent:shellclaw"
agent_name = "ShellClaw"
description = "C-native edge-AI ASAP agent (static manifest publish)."
public_base_url = "${PUBLIC_BASE}"

[hardware]
enabled = true
class = "edge_accelerator"
model = "jetson_orin_nano_super_8gb"
io = ["gpio", "i2c"]
EOF

BASE_URL="http://${HOST}:${PORT}"
HEALTH_URL="${BASE_URL}/health"
MANIFEST_URL="${BASE_URL}/.well-known/asap/manifest.json"

health_ok() {
	local code
	code="$(curl -sS -o /dev/null -w '%{http_code}' --connect-timeout 2 --max-time 5 "${HEALTH_URL}" 2>/dev/null || echo 000)"
	[[ "${code}" == "200" ]]
}

wait_for_health() {
	local deadline=$((SECONDS + TIMEOUT_SEC))
	while ((SECONDS < deadline)); do
		if health_ok; then
			return 0
		fi
		sleep 0.2
	done
	return 1
}

if [[ "${MANIFEST_DUMP_PRE_ROTATE:-}" == "1" ]]; then
	SHELLCLAW_BOARD="${BOARD}" "${BIN}" --rotate-keys --config "${CFG}" >/dev/null
fi

if [[ "${MANIFEST_DUMP_SKIP_START:-}" == "1" ]]; then
	if ! health_ok; then
		echo "dump_manifest: MANIFEST_DUMP_SKIP_START=1 but ${HEALTH_URL} not healthy" >&2
		exit 3
	fi
else
	if ! health_ok; then
		: >"${SHELLCLAW_HOME}/shellclaw.log"
		SHELLCLAW_BOARD="${BOARD}" SHELLCLAW_TEST_MODE=1 \
			"${BIN}" --config "${CFG}" >>"${SHELLCLAW_HOME}/shellclaw.log" 2>&1 &
		STARTED_PID=$!
		if ! wait_for_health; then
			echo "dump_manifest: gateway did not become healthy at ${HEALTH_URL} within ${TIMEOUT_SEC}s" >&2
			if [[ -f "${SHELLCLAW_HOME}/shellclaw.log" ]]; then
				echo "dump_manifest: last log lines:" >&2
				tail -n 20 "${SHELLCLAW_HOME}/shellclaw.log" >&2 || true
			fi
			exit 3
		fi
	fi
fi

TMP_JSON="$(mktemp)"
trap 'rm -f "${TMP_JSON}"; cleanup' EXIT

HTTP_CODE="$(curl -sS -o "${TMP_JSON}" -w '%{http_code}' \
	--connect-timeout 5 --max-time 30 "${MANIFEST_URL}")" || {
	echo "dump_manifest: curl failed for ${MANIFEST_URL}" >&2
	exit 4
}

if [[ "${HTTP_CODE}" != "200" ]]; then
	echo "dump_manifest: GET ${MANIFEST_URL} returned HTTP ${HTTP_CODE}" >&2
	cat "${TMP_JSON}" >&2 || true
	exit 4
fi

if ! grep -q '"manifest"' "${TMP_JSON}" || ! grep -q '"signature"' "${TMP_JSON}" || ! grep -q '"public_key"' "${TMP_JSON}"; then
	echo "dump_manifest: response is not a SignedManifest shape" >&2
	exit 4
fi

if [[ -n "${OUT_FILE}" ]]; then
	mkdir -p "$(dirname "${OUT_FILE}")"
	cp "${TMP_JSON}" "${OUT_FILE}"
else
	cat "${TMP_JSON}"
fi
