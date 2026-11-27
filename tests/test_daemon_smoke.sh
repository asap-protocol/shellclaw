#!/usr/bin/env bash
# Smoke daemon mode: HOME in a temp dir, verify pid/log and flock on double start.

set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

BIN="${SHELLCLAW_TEST_BIN:-${ROOT}/build/shellclaw}"
if [[ ! -x "${BIN}" ]]; then
	echo "test_daemon_smoke: build shellclaw first (${BIN})" >&2

	exit 1
fi

TMP_HOME="$(mktemp -d)"
trap 'chmod -R u+w "${TMP_HOME}" >/dev/null 2>&1 || true; rm -rf "${TMP_HOME}"' EXIT
export HOME="${TMP_HOME}"

mkdir -p "${HOME}/.shellclaw"
CFG="${HOME}/.shellclaw/config.toml"
cat <<'EOF_CFG' >"${CFG}"

[agent]
model = "daemon-smoke-model"

EOF_CFG

detect_out="$("${BIN}" --detect-board)"
if [[ -z "${detect_out}" || "${detect_out}" == *$'\n'* ]]; then
	echo "test_daemon_smoke: --detect-board expected single non-empty line, got: ${detect_out@Q}" >&2
	exit 1
fi

stub_out="$(SHELLCLAW_BOARD=stub "${BIN}" --detect-board)"
if [[ "${stub_out}" != "stub" ]]; then
	echo "test_daemon_smoke: SHELLCLAW_BOARD=stub expected 'stub', got: ${stub_out@Q}" >&2
	exit 1
fi

# systemd sets INVOCATION_ID and uses /dev/null for the old unit's stdin.
service_id=7d2d19cbe1f748b7be0651d680b95b51
SHELLCLAW_INSTALL_BIN="${BIN}" INVOCATION_ID="${service_id}" \
	"${BIN}" --config "${CFG}" </dev/null &
service_pid=$!
sleep 1
if ! kill -0 "${service_pid}" 2>/dev/null; then
	echo "test_daemon_smoke: ShellClaw exited after EOF in the old systemd unit" >&2
	exit 1
fi
kill "${service_pid}"
wait "${service_pid}"

# A systemd service can also pass a finite stream from its caller.
printf '/help\n' | INVOCATION_ID="${service_id}" "${BIN}" --config "${CFG}" >"${TMP_HOME}/pipe.log" &
pipe_pid=$!
for ((i = 0; i < 50; i++)); do
	kill -0 "${pipe_pid}" 2>/dev/null || break
	sleep 0.1
done
if kill -0 "${pipe_pid}" 2>/dev/null; then
	kill "${pipe_pid}"
	wait "${pipe_pid}" || true
	echo "test_daemon_smoke: ShellClaw stayed active after the service pipe closed" >&2
	exit 1
fi
wait "${pipe_pid}"
grep -Fq '/reset' "${TMP_HOME}/pipe.log"

"${BIN}" --daemon --config "${CFG}"

sleep 1
test -f "${HOME}/.shellclaw/shellclaw.pid"
test -f "${HOME}/.shellclaw/shellclaw.log"
running_pid="$(cat "${HOME}/.shellclaw/shellclaw.pid")"

if ! kill -0 "${running_pid}" 2>/dev/null; then
	echo "test_daemon_smoke: first daemon PID not alive" >&2
	exit 1
fi

set +e
"${BIN}" --daemon --config "${CFG}" >/dev/null 2>&1
set -e

sleep 1
if ! grep -Fq "another instance holds" "${HOME}/.shellclaw/shellclaw.log"; then
	echo "test_daemon_smoke: expected concurrent daemon lock message in log" >&2
	kill "${running_pid}"
	exit 1
fi

kill "${running_pid}"

wait "${running_pid}" 2>/dev/null || true

# --rotate-keys: isolated SHELLCLAW_HOME, backup on second rotation, new keys differ.
ROT_HOME="${TMP_HOME}/rotate-keys-home"
export SHELLCLAW_HOME="${ROT_HOME}"
mkdir -p "${SHELLCLAW_HOME}/keys"

rotate_out="$("${BIN}" --rotate-keys)"
if [[ "${rotate_out}" != "rotation complete; refresh your marketplace listing" ]]; then
	echo "test_daemon_smoke: --rotate-keys unexpected stdout: ${rotate_out@Q}" >&2
	exit 1
fi
test -f "${SHELLCLAW_HOME}/keys/ed25519.priv"
test -f "${SHELLCLAW_HOME}/keys/ed25519.pub"
cp "${SHELLCLAW_HOME}/keys/ed25519.priv" "${ROT_HOME}/priv1.bin"
cp "${SHELLCLAW_HOME}/keys/ed25519.pub" "${ROT_HOME}/pub1.bin"

shopt -s nullglob
bak_priv_before=("${SHELLCLAW_HOME}/keys"/ed25519.priv.bak.*)
bak_pub_before=("${SHELLCLAW_HOME}/keys"/ed25519.pub.bak.*)
if [[ ${#bak_priv_before[@]} -ne 0 || ${#bak_pub_before[@]} -ne 0 ]]; then
	echo "test_daemon_smoke: unexpected backup before second rotation" >&2
	exit 1
fi

rotate_out2="$("${BIN}" --rotate-keys)"
if [[ "${rotate_out2}" != "rotation complete; refresh your marketplace listing" ]]; then
	echo "test_daemon_smoke: second --rotate-keys unexpected stdout: ${rotate_out2@Q}" >&2
	exit 1
fi

bak_priv=("${SHELLCLAW_HOME}/keys"/ed25519.priv.bak.*)
bak_pub=("${SHELLCLAW_HOME}/keys"/ed25519.pub.bak.*)
if [[ ${#bak_priv[@]} -ne 1 ]]; then
	echo "test_daemon_smoke: expected one ed25519.priv.bak.* file, got ${#bak_priv[@]}" >&2
	exit 1
fi
if [[ ${#bak_pub[@]} -ne 1 ]]; then
	echo "test_daemon_smoke: expected one ed25519.pub.bak.* file, got ${#bak_pub[@]}" >&2
	exit 1
fi
if ! cmp -s "${bak_priv[0]}" "${ROT_HOME}/priv1.bin"; then
	echo "test_daemon_smoke: priv backup does not match pre-rotation key" >&2
	exit 1
fi
if ! cmp -s "${bak_pub[0]}" "${ROT_HOME}/pub1.bin"; then
	echo "test_daemon_smoke: pub backup does not match pre-rotation key" >&2
	exit 1
fi
if cmp -s "${SHELLCLAW_HOME}/keys/ed25519.pub" "${ROT_HOME}/pub1.bin"; then
	echo "test_daemon_smoke: rotated public key unchanged" >&2
	exit 1
fi
priv_mode="$(stat -c '%a' "${SHELLCLAW_HOME}/keys/ed25519.priv" 2>/dev/null || stat -f '%OLp' "${SHELLCLAW_HOME}/keys/ed25519.priv")"
pub_mode="$(stat -c '%a' "${SHELLCLAW_HOME}/keys/ed25519.pub" 2>/dev/null || stat -f '%OLp' "${SHELLCLAW_HOME}/keys/ed25519.pub")"
if [[ "${priv_mode}" != "600" || "${pub_mode}" != "600" ]]; then
	echo "test_daemon_smoke: expected key files mode 600, got priv=${priv_mode} pub=${pub_mode}" >&2
	exit 1
fi

echo "test_daemon_smoke: OK"
