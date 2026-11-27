#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
sandbox="$(mktemp -d)"
trap 'rm -rf "${sandbox}"' EXIT

export HOME="${sandbox}/home"
export XDG_CONFIG_HOME="${HOME}/.config"
mkdir -p "${HOME}"

fake_bin="${sandbox}/bin"
mkdir -p "${fake_bin}"
fake_shellclaw="${fake_bin}/shellclaw"
etc_dir="${sandbox}/etc /shellclaw%h"
state_dir="${sandbox}/claw & state"
unset SHELLCLAW_INSTALL_BIN SHELLCLAW_HOME SHELLCLAW_ETC_DIR
unit_dir="${XDG_CONFIG_HOME}/systemd/user"
jetson_marker="Phi-3-mini-4k-instruct-Q4_K_M.gguf"
rpi_marker="tinyllama-1.1b-chat-Q4_K_M.gguf"

write_fake_shellclaw() {
	local board="$1"
	cat >"${fake_shellclaw}" <<EOF
#!/usr/bin/env bash
if [[ "\${1:-}" == "--detect-board" ]]; then
	printf '%s\n' "${board}"
	exit 0
fi
echo "fake shellclaw: unexpected args: \$*" >&2
exit 1
EOF
	chmod +x "${fake_shellclaw}"
}

run_install_for_board() {
	local board="$1"
	write_fake_shellclaw "${board}"
	rm -rf "${etc_dir}"
	mkdir -p "${etc_dir}"
	SHELLCLAW_INSTALL_BIN="${fake_shellclaw}" SHELLCLAW_HOME="${state_dir}" \
		SHELLCLAW_ETC_DIR="${etc_dir}" \
		SHELLCLAW_INSTALL_NONINTERACTIVE=1 \
		bash "${ROOT}/scripts/install.sh"
}

assert_llama_env_marker() {
	local marker="$1"
	local dest="${etc_dir}/llama-server.env"
	if ! test -f "${dest}"; then
		echo "test_install_script: missing ${dest}" >&2
		exit 1
	fi
	if ! grep -Fq "${marker}" "${dest}"; then
		echo "test_install_script: ${dest} missing marker ${marker@Q}" >&2
		exit 1
	fi
}

assert_units_installed() {
	test -f "${unit_dir}/shellclaw.service"
	test -f "${unit_dir}/llama-server.service"
	grep -Fx "EnvironmentFile=${etc_dir//%/%%}/llama-server.env" "${unit_dir}/llama-server.service"
	grep -F "ExecStart=:/usr/bin/env -- \"${fake_shellclaw}\" --config \"${state_dir}/config.toml\"" "${unit_dir}/shellclaw.service"
	grep -Fx "EnvironmentFile=-${state_dir}/.env" "${unit_dir}/shellclaw.service"
	grep -Fx "db_path = \"${state_dir}/memory.db\"" "${state_dir}/config.toml"
}

run_install_for_board "jetson_orin_nano"
assert_units_installed
assert_llama_env_marker "${jetson_marker}"

run_install_for_board "rpi_zero2w"
assert_units_installed
assert_llama_env_marker "${rpi_marker}"

run_install_for_board "stub"
assert_units_installed
assert_llama_env_marker "${jetson_marker}"

chmod u-w "${fake_bin}"
rc=0
SHELLCLAW_INSTALL_BIN="${fake_shellclaw}" SHELLCLAW_INSTALL_NONINTERACTIVE=1 \
	bash "${ROOT}/scripts/install.sh" || rc=$?
chmod u+w "${fake_bin}"
test "${rc}" = 0

default_bin="${HOME}/.local/bin/shellclaw"
mkdir -p "$(dirname "${default_bin}")"
cp "${fake_shellclaw}" "${default_bin}"
SHELLCLAW_INSTALL_NONINTERACTIVE=1 bash "${ROOT}/scripts/install.sh"
grep -Fx "EnvironmentFile=${HOME}/.shellclaw/llama-server.env" "${unit_dir}/llama-server.service"
test -f "${HOME}/.shellclaw/llama-server.env"
test -f "${HOME}/.shellclaw/config.toml"
test -f "${HOME}/.shellclaw/.env"

for file in config.toml .env llama-server.env; do
	printf '# local file\n' >"${HOME}/.shellclaw/${file}"
done
SHELLCLAW_INSTALL_NONINTERACTIVE=1 bash "${ROOT}/scripts/install.sh"
for file in config.toml .env llama-server.env; do
	grep -Fx '# local file' "${HOME}/.shellclaw/${file}"
done

invalid_xdg="${sandbox}/invalid-xdg"
touch "${sandbox}/not-a-directory"
if XDG_CONFIG_HOME="${invalid_xdg}" SHELLCLAW_ETC_DIR="${sandbox}/not-a-directory" \
		SHELLCLAW_INSTALL_NONINTERACTIVE=1 bash "${ROOT}/scripts/install.sh"; then
	echo "test_install_script: invalid destination accepted" >&2
	exit 1
fi
test ! -e "${invalid_xdg}/systemd/user/shellclaw.service"
test ! -e "${invalid_xdg}/systemd/user/llama-server.service"

if SHELLCLAW_HOME="${sandbox}/state*" SHELLCLAW_INSTALL_NONINTERACTIVE=1 \
		bash "${ROOT}/scripts/install.sh"; then
	echo "test_install_script: wildcard in environment file path accepted" >&2
	exit 1
fi
test ! -e "${sandbox}/state*"

echo "test_install_script: OK"
