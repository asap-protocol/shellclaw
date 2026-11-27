#!/usr/bin/env bash
# Install ShellClaw and its systemd user units.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
UNIT_DIR="${XDG_CONFIG_HOME:-$HOME/.config}/systemd/user"
INSTALL_BIN="${SHELLCLAW_INSTALL_BIN:-${HOME%/}/.local/bin/shellclaw}"
BUILT_BIN="${ROOT}/build/shellclaw"
STATE_DIR="${SHELLCLAW_HOME:-${HOME%/}/.shellclaw}"
LLAMA_ENV_DIR="${SHELLCLAW_ETC_DIR:-${STATE_DIR}}"
LLAMA_ENV_DEST="${LLAMA_ENV_DIR}/llama-server.env"
LEGACY_LLAMA_ENV="/etc/shellclaw/llama-server.env"
JETSON_ENV="${ROOT}/systemd/llama-server.jetson.env"
RPI_ENV="${ROOT}/systemd/llama-server.rpi.env"

validate_path() {
	if [[ "$1" != /* || "$1" == *[[:cntrl:]]* ]]; then
		printf 'Use an absolute path without control characters for %s.\n' "$2" >&2
		exit 1
	fi
}

validate_env_dir() {
	validate_path "$1" "$2"
	if [[ "$1" == *[\*\?\[\\]* ]]; then
		printf 'Use a path without wildcard characters for %s.\n' "$2" >&2
		exit 1
	fi
}

quote_unit_value() {
	local value="${1//%/%%}"
	value="${value//\\/\\\\}"
	value="${value//\"/\\\"}"
	printf '"%s"' "${value}"
}

render_unit() {
	local line
	while IFS= read -r line; do
		case "${line}" in
		Environment=SHELLCLAW_HOME=*)
			printf 'Environment=%s\n' "$(quote_unit_value "SHELLCLAW_HOME=${STATE_DIR}")"
			;;
		EnvironmentFile=-%h/.shellclaw/.env)
			printf 'EnvironmentFile=-%s/.env\n' "${STATE_DIR//%/%%}"
			;;
		EnvironmentFile=%h/.shellclaw/llama-server.env)
			printf 'EnvironmentFile=%s\n' "${LLAMA_ENV_DEST//%/%%}"
			;;
		ExecStart=%h/.local/bin/shellclaw*)
			# systemd limits executable names. env passes the full path as an argument.
			printf 'ExecStart=:/usr/bin/env -- %s --config %s%s\n' "$(quote_unit_value "${INSTALL_BIN}")" \
				"$(quote_unit_value "${STATE_DIR}/config.toml")" "${line#*%h/.shellclaw/config.toml}"
			;;
		*) printf '%s\n' "${line}" ;;
		esac
	done <"$1"
}

install_config() {
	local line
	local default_prefix='~/.shellclaw/'
	local state_prefix="${STATE_DIR//\\/\\\\}"
	state_prefix="${state_prefix//\"/\\\"}/"
	while IFS= read -r line; do
		printf '%s\n' "${line//"${default_prefix}"/"${state_prefix}"}"
	done <"${ROOT}/config.example.toml" >"${STATE_DIR}/config.toml"
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

install_llama_env() (
	local stage
	stage="$(mktemp "${LLAMA_ENV_DIR}/.llama-server.env.XXXXXX")"
	trap 'rm -f -- "${stage}"' EXIT
	install -m 0600 "$1" "${stage}"
	mv -n -- "${stage}" "${LLAMA_ENV_DEST}"
)

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
	read -r -p "Download the default model (${model_key})? [y/N] " reply || return 0
	if [[ "${reply}" =~ ^[Yy]$ ]]; then
		bash "${ROOT}/scripts/download_model.sh" "${model_key}"
	fi
}

validate_path "${UNIT_DIR}" XDG_CONFIG_HOME
validate_path "${INSTALL_BIN}" SHELLCLAW_INSTALL_BIN
validate_env_dir "${STATE_DIR}" SHELLCLAW_HOME
validate_env_dir "${LLAMA_ENV_DIR}" SHELLCLAW_ETC_DIR

directories=("${STATE_DIR}" "${UNIT_DIR}")
if [[ ! -e "${LLAMA_ENV_DEST}" ]]; then
	directories+=("${LLAMA_ENV_DIR}")
fi
selected_bin="${INSTALL_BIN}"
copy_binary=0
if [[ ! -x "${INSTALL_BIN}" || ( -z "${SHELLCLAW_INSTALL_BIN:-}" && -x "${BUILT_BIN}" ) ]]; then
	if [[ ! -x "${BUILT_BIN}" ]]; then
		echo "Run make shellclaw before installation." >&2
		exit 1
	fi
	selected_bin="${BUILT_BIN}"
	copy_binary=1
	directories+=("$(dirname "${INSTALL_BIN}")")
fi

# Pass --headless before --help so the binary must parse both options.
if ! "${selected_bin}" --headless --help >/dev/null 2>&1; then
	printf 'ShellClaw at %s does not accept --headless.\n' "${selected_bin}" >&2
	echo 'Run make shellclaw. If you set SHELLCLAW_INSTALL_BIN, use a compatible binary.' >&2
	exit 1
fi

llama_env_source=""
if [[ ! -e "${LLAMA_ENV_DEST}" && -z "${SHELLCLAW_ETC_DIR:-}" &&
	( -e "${LEGACY_LLAMA_ENV}" || -L "${LEGACY_LLAMA_ENV}" ) ]]; then
	if [[ ! -f "${LEGACY_LLAMA_ENV}" || ! -r "${LEGACY_LLAMA_ENV}" ]]; then
		printf 'Cannot read the previous llama-server environment at %s.\n' "${LEGACY_LLAMA_ENV}" >&2
		exit 1
	fi
	llama_env_source="${LEGACY_LLAMA_ENV}"
fi

umask 077
mkdir -p "${directories[@]}"
for dir in "${directories[@]}"; do
	if [[ ! -w "${dir}" || ! -x "${dir}" ]]; then
		printf 'Cannot write to %s.\n' "${dir}" >&2
		exit 1
	fi
done
for path in "${INSTALL_BIN}" "${STATE_DIR}/config.toml" "${STATE_DIR}/.env" "${LLAMA_ENV_DEST}" \
	"${UNIT_DIR}/shellclaw.service" "${UNIT_DIR}/llama-server.service"; do
	if [[ ( -e "${path}" || -L "${path}" ) && ! -f "${path}" ]]; then
		printf 'Expected a regular file at %s.\n' "${path}" >&2
		exit 1
	fi
done

if ((copy_binary)); then
	install -m 0755 "${selected_bin}" "${INSTALL_BIN}"
fi
if [[ ! -e "${STATE_DIR}/config.toml" ]]; then
	install_config
fi
if [[ ! -e "${STATE_DIR}/.env" ]]; then
	install -m 0600 "${ROOT}/.env.example" "${STATE_DIR}/.env"
fi
board_id="$(detect_board_id)"
if [[ ! -e "${LLAMA_ENV_DEST}" ]]; then
	install_llama_env "${llama_env_source:-$(board_llama_env_source "${board_id}")}"
fi
unit_stage="$(mktemp -d)"
trap 'rm -rf -- "${unit_stage}"' EXIT
for unit in shellclaw.service llama-server.service; do
	render_unit "${ROOT}/systemd/${unit}" >"${unit_stage}/${unit}"
	install -m 0644 "${unit_stage}/${unit}" "${UNIT_DIR}/${unit}"
done

echo "Installed user units in ${UNIT_DIR}."
echo "ShellClaw binary is at ${INSTALL_BIN}."
echo "Edit ${STATE_DIR}/config.toml before you start ShellClaw."
echo "For a cloud provider, set its API key in ${STATE_DIR}/.env."
echo "  systemctl --user daemon-reload"
echo "  systemctl --user enable --now shellclaw.service"
echo "If you use local inference, build llama-server and download a model."
echo "Set MODEL in ${LLAMA_ENV_DEST} to the model path."
echo "  systemctl --user enable --now llama-server.service"

maybe_prompt_download_model "${board_id}"
