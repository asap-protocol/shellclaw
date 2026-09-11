#!/usr/bin/env bash
# Install helper: copy systemd user units next to ~/.config/systemd/user and print enable steps.
#
# Purpose: `./scripts/install.sh` — aligns with Phase 4 PRD §4.4.30; Phase 5 Task 3.5 adds
# board-specific llama-server env under /etc/shellclaw (override via SHELLCLAW_ETC_DIR).
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
UNIT_DIR="${XDG_CONFIG_HOME:-$HOME/.config}/systemd/user"
INSTALL_BIN="${SHELLCLAW_INSTALL_BIN:-${HOME%/}/.local/bin/shellclaw}"
SHELLCLAW_HOME="${SHELLCLAW_HOME:-${HOME%/}/.shellclaw}"
LLAMA_ENV_DIR="${SHELLCLAW_ETC_DIR:-/etc/shellclaw}"
LLAMA_ENV_DEST="${LLAMA_ENV_DIR}/llama-server.env"
JETSON_ENV="${ROOT}/systemd/llama-server.jetson.env"
RPI_ENV="${ROOT}/systemd/llama-server.rpi.env"

install_llama_env_file() {
	local src_template="$1"
	mkdir -p "${LLAMA_ENV_DIR}"
	if [[ -w "${LLAMA_ENV_DIR}" ]]; then
		install -m 0644 "${src_template}" "${LLAMA_ENV_DEST}"
	elif [[ "${LLAMA_ENV_DIR}" == /etc/shellclaw ]] && command -v sudo >/dev/null 2>&1; then
		sudo install -m 0644 "${src_template}" "${LLAMA_ENV_DEST}"
	else
		echo "Cannot write ${LLAMA_ENV_DEST}; set SHELLCLAW_ETC_DIR or run with sufficient permissions." >&2
		exit 1
	fi
}

detect_board_id() {
	if [[ ! -x "${INSTALL_BIN}" ]]; then
		echo "unknown"
		return 0
	fi
	local out
	out="$("${INSTALL_BIN}" --detect-board 2>/dev/null || true)"
	out="${out//$'\r'/}"
	out="${out%%$'\n'*}"
	if [[ -z "${out}" ]]; then
		echo "unknown"
	else
		printf '%s' "${out}"
	fi
}

board_llama_env_source() {
	local board="$1"
	case "${board}" in
	jetson_orin_nano)
		printf '%s' "${JETSON_ENV}"
		;;
	rpi_zero2w)
		printf '%s' "${RPI_ENV}"
		;;
	stub)
		printf '%s' "${JETSON_ENV}"
		;;
	unknown|*)
		printf '%s' "${JETSON_ENV}"
		;;
	esac
}

board_download_model_key() {
	local board="$1"
	case "${board}" in
	rpi_zero2w)
		printf '%s' "tinyllama"
		;;
	jetson_orin_nano|stub|unknown|*)
		printf '%s' "phi3"
		;;
	esac
}

maybe_prompt_download_model() {
	local board="$1"
	local model_key
	model_key="$(board_download_model_key "${board}")"
	if [[ "${SHELLCLAW_INSTALL_NONINTERACTIVE:-}" == "1" ]]; then
		return 0
	fi
	if [[ ! -t 0 ]]; then
		return 0
	fi
	local reply=""
	read -r -p "Download default GGUF now via scripts/download_model.sh (${model_key})? [y/N] " reply
	if [[ "${reply}" =~ ^[Yy]$ ]]; then
		bash "${ROOT}/scripts/download_model.sh" "${model_key}"
	fi
}

mkdir -p "${UNIT_DIR}"
install -m 0644 "${ROOT}/systemd/shellclaw.service" "${UNIT_DIR}/shellclaw.service"
install -m 0644 "${ROOT}/systemd/llama-server.service" "${UNIT_DIR}/llama-server.service"

board_id="$(detect_board_id)"
env_src="$(board_llama_env_source "${board_id}")"
install_llama_env_file "${env_src}"

case "${board_id}" in
jetson_orin_nano)
	echo "Installed llama-server env (Jetson) at ${LLAMA_ENV_DEST}"
	;;
rpi_zero2w)
	echo "Installed llama-server env (RPi) at ${LLAMA_ENV_DEST}"
	;;
stub)
	echo "Board stub: installed Jetson llama-server env template at ${LLAMA_ENV_DEST} (dev/CI)"
	;;
unknown|*)
	echo "Board unknown: installed Jetson llama-server env template at ${LLAMA_ENV_DEST}"
	;;
esac

maybe_prompt_download_model "${board_id}"

echo "Installed units into ${UNIT_DIR}"
echo ""
if [[ ! -x "${INSTALL_BIN}" ]]; then
	echo "Warning: ShellClaw binary not found or not executable at ${INSTALL_BIN}" >&2
	echo "Build or copy the binary before starting the service (see README)." >&2
	echo ""
fi
echo "Environment (override before enable if needed):"
echo "  SHELLCLAW_INSTALL_BIN=${INSTALL_BIN}"
echo "  SHELLCLAW_HOME=${SHELLCLAW_HOME}"
echo "  SHELLCLAW_ETC_DIR=${LLAMA_ENV_DIR} (llama-server.env)"
echo ""
echo "Next:"
echo '  Prefix `systemctl` with `sudo` only if you use system-wide systemd instead of `--user`.'
echo "  systemctl --user daemon-reload"
echo "  systemctl --user enable --now shellclaw.service"
echo "Optional (local inference):"
echo "  Build llama-server: ./scripts/build_llama_jetson.sh or ./scripts/build_llama_rpi.sh"
echo "  Download model: ./scripts/download_model.sh phi3   # Jetson default"
echo "                  ./scripts/download_model.sh tinyllama   # RPi default"
echo "  systemctl --user enable --now llama-server.service"
echo "  (uses ${LLAMA_ENV_DEST}; restart after editing MODEL=)"
