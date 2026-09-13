#!/usr/bin/env bash
#
# Build llama.cpp llama-server (CPU native) for Raspberry Pi (aarch64).
# Phase 5 PRD §4.5 — deployment only; ShellClaw providers/local.c is unchanged.
#
# Pinned upstream: ggml-org/llama.cpp tag b9087 (2026-05-09).
# Shipped in v1.0 for completeness; full validation on hardware in Phase 6.
#
# Usage:
#   ./scripts/build_llama_rpi.sh
#
# Environment (optional):
#   LLAMA_SRC_DIR     — clone/build tree (default: ${HOME}/src/llama.cpp)
#   INSTALL_BIN_DIR   — install destination (default: /usr/local/bin)
#   SKIP_ARCH_CHECK=1 — allow non-aarch64 hosts (for script lint only; build may still fail)
#   FORCE_REBUILD=1   — wipe build dir and rebuild even when the tag is already checked out
#
# Verify on device:
#   llama-server --version   # must NOT mention CUDA / cuBLAS / GPU
#

set -euo pipefail

LLAMA_CPP_TAG="b9087"
LLAMA_CPP_REPO="https://github.com/ggml-org/llama.cpp.git"
LLAMA_SRC_DIR="${LLAMA_SRC_DIR:-${HOME}/src/llama.cpp}"
INSTALL_BIN_DIR="${INSTALL_BIN_DIR:-/usr/local/bin}"
BUILD_DIR="${LLAMA_SRC_DIR}/build"
JOBS="$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)"

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

assert_rpi_host() {
	if [[ "${SKIP_ARCH_CHECK:-}" == "1" ]]; then
		return 0
	fi
	local machine
	machine="$(uname -m)"
	if [[ "${machine}" != "aarch64" ]]; then
		die "expected aarch64 Raspberry Pi host (got ${machine}); set SKIP_ARCH_CHECK=1 only to syntax-check this script"
	fi
}

install_binary() {
	local staged="$1"
	local dest="${INSTALL_BIN_DIR}/llama-server"
	if [[ "$(id -u)" -eq 0 ]]; then
		install -m 0755 "${staged}" "${dest}"
	else
		sudo install -m 0755 "${staged}" "${dest}"
	fi
}

verify_cpu_build() {
	local bin="${INSTALL_BIN_DIR}/llama-server"
	[[ -x "${bin}" ]] || die "installed binary missing: ${bin}"
	local version_out
	version_out="$("${bin}" --version 2>&1)" || die "llama-server --version failed"
	if printf '%s\n' "${version_out}" | grep -Eiq 'cuda|cublas|gpu'; then
		die "llama-server --version reports a CUDA/GPU build (expected CPU native):${version_out}"
	fi
	log "CPU native build confirmed:"
	printf '%s\n' "${version_out}"
}

fetch_sources() {
	require_cmd git
	mkdir -p "$(dirname "${LLAMA_SRC_DIR}")"
	if [[ ! -d "${LLAMA_SRC_DIR}/.git" ]]; then
		log "Cloning ${LLAMA_CPP_REPO} (tag ${LLAMA_CPP_TAG}) into ${LLAMA_SRC_DIR}"
		git clone --depth 1 --branch "${LLAMA_CPP_TAG}" "${LLAMA_CPP_REPO}" "${LLAMA_SRC_DIR}"
		return 0
	fi
	log "Updating existing tree at ${LLAMA_SRC_DIR}"
	git -C "${LLAMA_SRC_DIR}" fetch --depth 1 origin "refs/tags/${LLAMA_CPP_TAG}:refs/tags/${LLAMA_CPP_TAG}" 2>/dev/null \
		|| git -C "${LLAMA_SRC_DIR}" fetch --tags origin
	git -C "${LLAMA_SRC_DIR}" checkout -f "${LLAMA_CPP_TAG}"
}

configure_and_build() {
	require_cmd cmake
	if [[ "${FORCE_REBUILD:-}" == "1" && -d "${BUILD_DIR}" ]]; then
		rm -rf "${BUILD_DIR}"
	fi
	log "Configuring CMake (GGML_CUDA=OFF, GGML_NATIVE=ON)"
	cmake -S "${LLAMA_SRC_DIR}" -B "${BUILD_DIR}" \
		-DGGML_CUDA=OFF \
		-DGGML_NATIVE=ON \
		-DCMAKE_BUILD_TYPE=Release
	log "Building llama-server (${JOBS} jobs)"
	cmake --build "${BUILD_DIR}" --config Release --target llama-server -j "${JOBS}"
	[[ -x "${BUILD_DIR}/bin/llama-server" ]] \
		|| die "build succeeded but ${BUILD_DIR}/bin/llama-server is missing"
}

main() {
	assert_rpi_host
	require_cmd cmake
	fetch_sources
	configure_and_build
	log "Installing to ${INSTALL_BIN_DIR}/llama-server"
	install_binary "${BUILD_DIR}/bin/llama-server"
	verify_cpu_build
	log "Done. Enable systemd unit after env file is installed (slice 03 task 3.4/3.5)."
}

main "$@"
