#!/usr/bin/env bash
#
# Download default GGUF models for ShellClaw local inference (Phase 5 Q-MODEL).
# Deployment only — providers/local.c is unchanged.
#
# Pinned Hugging Face resolve URLs (2026-05-24):
#   phi3 (Phi-3-mini-4k-instruct-Q4_K_M.gguf):
#     https://huggingface.co/QuantFactory/Phi-3-mini-4k-instruct-GGUF/resolve/main/Phi-3-mini-4k-instruct.Q4_K_M.gguf
#   tinyllama (tinyllama-1.1b-chat-Q4_K_M.gguf):
#     https://huggingface.co/TheBloke/TinyLlama-1.1B-Chat-v1.0-GGUF/resolve/main/tinyllama-1.1b-chat-v1.0.Q4_K_M.gguf
#
# Fallback if HF is unreachable (documented; not auto-tried):
#   phi3: https://huggingface.co/microsoft/Phi-3-mini-4k-instruct-gguf/resolve/main/Phi-3-mini-4k-instruct-q4.gguf
#         (upstream name differs; same Q4_K_M quant — save as Phi-3-mini-4k-instruct-Q4_K_M.gguf)
#   tinyllama: same TheBloke repo via `huggingface-cli download TheBloke/TinyLlama-1.1B-Chat-v1.0-GGUF tinyllama-1.1b-chat-v1.0.Q4_K_M.gguf`
#
# Official blob SHA256 is not vendored here: Hugging Face file hashes can change
# with repo revisions. Do not invent a digest. For production installs set
# EXPECTED_SHA256, or PHI3_SHA256 / TINYLLAMA_SHA256, from the HF file metadata page.
#
# Usage:
#   ./scripts/download_model.sh <model-key>
#
# Model keys:
#   phi3 | phi-3-mini  — Jetson default (Phi-3-mini-4k-instruct-Q4_K_M.gguf)
#   tinyllama          — RPi / fast-first-boot default (tinyllama-1.1b-chat-Q4_K_M.gguf)
#
# Environment (optional):
#   MODEL_DIR          — destination directory (default: /var/lib/shellclaw/models)
#   EXPECTED_SHA256    — if set, verify on skip-if-present and after download
#   PHI3_SHA256        — default EXPECTED_SHA256 for phi3 / phi-3-mini (optional)
#   TINYLLAMA_SHA256   — default EXPECTED_SHA256 for tinyllama (optional)
#   SKIP_DOWNLOAD=1    — never fetch; only verify or skip existing files (tests)
#   DOWNLOAD_CMD       — override downloader: receives URL and temp path (tests)
#

set -euo pipefail

MODEL_DIR="${MODEL_DIR:-/var/lib/shellclaw/models}"

PHI3_FILENAME="Phi-3-mini-4k-instruct-Q4_K_M.gguf"
PHI3_URL="https://huggingface.co/QuantFactory/Phi-3-mini-4k-instruct-GGUF/resolve/main/Phi-3-mini-4k-instruct.Q4_K_M.gguf"

TINYLLAMA_FILENAME="tinyllama-1.1b-chat-Q4_K_M.gguf"
TINYLLAMA_URL="https://huggingface.co/TheBloke/TinyLlama-1.1B-Chat-v1.0-GGUF/resolve/main/tinyllama-1.1b-chat-v1.0.Q4_K_M.gguf"

log() {
	printf '%s\n' "$*"
}

die() {
	printf '%s: %s\n' "${0}" "$*" >&2
	exit 1
}

require_cmd() {
	command -v "$1" >/dev/null 2>&1 || die "missing required command: $1"
}

file_sha256() {
	local path="$1"
	if command -v sha256sum >/dev/null 2>&1; then
		sha256sum "${path}" | awk '{print $1}'
	else
		shasum -a 256 "${path}" | awk '{print $1}'
	fi
}

verify_expected_sha256() {
	local path="$1"
	local expected="${EXPECTED_SHA256:-}"
	if [[ -z "${expected}" ]]; then
		return 0
	fi
	local actual
	actual="$(file_sha256 "${path}")"
	if [[ "${actual}" != "${expected}" ]]; then
		die "SHA256 mismatch for ${path} (expected ${expected}, got ${actual})"
	fi
}

file_is_ready() {
	local path="$1"
	[[ -f "${path}" ]] || return 1
	[[ -s "${path}" ]] || return 1
	return 0
}

ensure_model_dir() {
	if [[ -d "${MODEL_DIR}" ]]; then
		return 0
	fi
	if [[ "$(id -u)" -eq 0 ]]; then
		mkdir -p "${MODEL_DIR}"
		chmod 0755 "${MODEL_DIR}"
	else
		sudo mkdir -p "${MODEL_DIR}"
		sudo chmod 0755 "${MODEL_DIR}"
	fi
}

install_model_file() {
	local src="$1"
	local dest="$2"
	if [[ "$(id -u)" -eq 0 ]]; then
		install -m 0644 "${src}" "${dest}"
	else
		if [[ "${MODEL_DIR}" == /var/lib/* ]]; then
			sudo install -m 0644 "${src}" "${dest}"
		else
			install -m 0644 "${src}" "${dest}"
		fi
	fi
}

curl_tls() {
	curl --proto '=https' --tlsv1.2 --proto-redir '=https' -fsSL "$@"
}

download_to_temp() {
	local url="$1"
	local tmp="$2"
	if [[ -n "${DOWNLOAD_CMD:-}" ]]; then
		"${DOWNLOAD_CMD}" "${url}" "${tmp}"
		return 0
	fi
	require_cmd curl
	curl_tls -o "${tmp}" "${url}"
}

resolve_model() {
	local key="${1:-}"
	case "${key}" in
		phi3 | phi-3-mini)
			MODEL_FILENAME="${PHI3_FILENAME}"
			MODEL_URL="${PHI3_URL}"
			if [[ -z "${EXPECTED_SHA256:-}" && -n "${PHI3_SHA256:-}" ]]; then
				EXPECTED_SHA256="${PHI3_SHA256}"
			fi
			;;
		tinyllama)
			MODEL_FILENAME="${TINYLLAMA_FILENAME}"
			MODEL_URL="${TINYLLAMA_URL}"
			if [[ -z "${EXPECTED_SHA256:-}" && -n "${TINYLLAMA_SHA256:-}" ]]; then
				EXPECTED_SHA256="${TINYLLAMA_SHA256}"
			fi
			;;
		'')
			die "usage: ${0} <model-key>  (phi3 | phi-3-mini | tinyllama)"
			;;
		*)
			die "unknown model key: ${key} (supported: phi3, phi-3-mini, tinyllama)"
			;;
	esac
}

maybe_skip_existing() {
	local dest="$1"
	if ! file_is_ready "${dest}"; then
		return 1
	fi
	verify_expected_sha256 "${dest}"
	log "Model already present (skipping download): ${dest}"
	return 0
}

download_model() {
	local dest="${MODEL_DIR}/${MODEL_FILENAME}"
	ensure_model_dir

	if maybe_skip_existing "${dest}"; then
		return 0
	fi

	if [[ "${SKIP_DOWNLOAD:-}" == "1" ]]; then
		die "model missing or invalid at ${dest} and SKIP_DOWNLOAD=1"
	fi

	local tmp
	tmp="$(mktemp "${MODEL_DIR}/.${MODEL_FILENAME}.XXXXXX")"
	trap 'rm -f "${tmp}"' RETURN

	log "Downloading ${MODEL_FILENAME} from Hugging Face"
	download_to_temp "${MODEL_URL}" "${tmp}"
	verify_expected_sha256 "${tmp}"
	install_model_file "${tmp}" "${dest}"
	trap - RETURN
	rm -f "${tmp}"

	verify_expected_sha256 "${dest}"
	log "Installed: ${dest}"
}

main() {
	resolve_model "${1:-}"
	download_model
}

main "$@"
