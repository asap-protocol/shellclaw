#!/usr/bin/env bash
#
# Build llama.cpp llama-server with CUDA for NVIDIA Jetson Orin (Ampere sm_87).
# Phase 5 PRD §4.5 — deployment only; ShellClaw providers/local.c is unchanged.
#
# Pinned upstream: ggml-org/llama.cpp tag b9087 (2026-05-09).
# Expect ~8 minutes on Jetson Orin Nano Super with active cooling (nproc parallel jobs).
#
# Usage:
#   ./scripts/build_llama_jetson.sh
#
# Environment (optional):
#   LLAMA_SRC_DIR     — clone/build tree (default: ${HOME}/src/llama.cpp)
#   INSTALL_BIN_DIR   — install destination (default: /usr/local/bin)
#   CMAKE_CUDA_COMPILER — nvcc path (default: /usr/local/cuda/bin/nvcc)
#   SKIP_ARCH_CHECK=1 — allow non-aarch64 hosts (for script lint only; build will still fail without CUDA)
#   FORCE_REBUILD=1   — wipe build dir and rebuild even when the tag is already checked out
#
# Verify on device:
#   llama-server --version   # must mention CUDA / cuBLAS
#

set -euo pipefail

LLAMA_CPP_TAG="b9087"
LLAMA_CPP_REPO="https://github.com/ggml-org/llama.cpp.git"
LLAMA_SRC_DIR="${LLAMA_SRC_DIR:-${HOME}/src/llama.cpp}"
INSTALL_BIN_DIR="${INSTALL_BIN_DIR:-/usr/local/bin}"
NVCC="${CMAKE_CUDA_COMPILER:-/usr/local/cuda/bin/nvcc}"
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

assert_jetson_host() {
	if [[ "${SKIP_ARCH_CHECK:-}" == "1" ]]; then
		return 0
	fi
	local machine
	machine="$(uname -m)"
	if [[ "${machine}" != "aarch64" ]]; then
		die "expected aarch64 Jetson host (got ${machine}); set SKIP_ARCH_CHECK=1 only to syntax-check this script"
	fi
}

assert_cuda_toolchain() {
	[[ -x "${NVCC}" ]] || die "nvcc not found or not executable at ${NVCC} (set CMAKE_CUDA_COMPILER)"
	if [[ -z "${SKIP_ARCH_CHECK:-}" ]]; then
		"${NVCC}" --version >/dev/null 2>&1 || die "nvcc --version failed"
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

verify_cuda_build() {
	local bin="${INSTALL_BIN_DIR}/llama-server"
	[[ -x "${bin}" ]] || die "installed binary missing: ${bin}"
	local version_out
	version_out="$("${bin}" --version 2>&1)" || die "llama-server --version failed"
	if ! printf '%s\n' "${version_out}" | grep -Eiq 'cuda|cublas|gpu'; then
		die "llama-server --version does not report a CUDA build:${version_out}"
	fi
	log "CUDA build confirmed:"
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
	log "Configuring CMake (GGML_CUDA=ON, sm_87, nvcc=${NVCC})"
	cmake -S "${LLAMA_SRC_DIR}" -B "${BUILD_DIR}" \
		-DGGML_CUDA=ON \
		-DCMAKE_CUDA_ARCHITECTURES=87 \
		-DCMAKE_CUDA_COMPILER="${NVCC}" \
		-DCMAKE_BUILD_TYPE=Release
	log "Building llama-server (${JOBS} jobs) — expect ~8 min on Jetson with cooler"
	cmake --build "${BUILD_DIR}" --config Release --target llama-server -j "${JOBS}"
	[[ -x "${BUILD_DIR}/bin/llama-server" ]] \
		|| die "build succeeded but ${BUILD_DIR}/bin/llama-server is missing"
}

main() {
	assert_jetson_host
	assert_cuda_toolchain
	require_cmd cmake
	fetch_sources
	configure_and_build
	log "Installing to ${INSTALL_BIN_DIR}/llama-server"
	install_binary "${BUILD_DIR}/bin/llama-server"
	verify_cuda_build
	log "Done. Enable systemd unit after env file is installed (slice 03 task 3.4/3.5)."
}

main "$@"
