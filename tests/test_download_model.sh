#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SCRIPT="${ROOT}/scripts/download_model.sh"
FIXTURE="${ROOT}/tests/fixtures/tinyllama-fixture.gguf"

if [[ ! -f "${FIXTURE}" ]]; then
	echo "test_download_model: missing fixture ${FIXTURE}" >&2
	exit 1
fi

if command -v sha256sum >/dev/null 2>&1; then
	FIXTURE_SHA="$(sha256sum "${FIXTURE}" | awk '{print $1}')"
else
	FIXTURE_SHA="$(shasum -a 256 "${FIXTURE}" | awk '{print $1}')"
fi

sandbox="$(mktemp -d)"
trap 'rm -rf "${sandbox}"' EXIT

model_dir="${sandbox}/models"
mkdir -p "${model_dir}"

export MODEL_DIR="${model_dir}"
export SKIP_DOWNLOAD=1
export EXPECTED_SHA256="${FIXTURE_SHA}"

dest="${model_dir}/tinyllama-1.1b-chat-Q4_K_M.gguf"

# Missing file with SKIP_DOWNLOAD must fail.
if bash "${SCRIPT}" tinyllama; then
	echo "test_download_model: expected failure when model missing and SKIP_DOWNLOAD=1" >&2
	exit 1
fi

cp -f "${FIXTURE}" "${dest}"

# Matching checksum: idempotent skip.
bash "${SCRIPT}" tinyllama
bash "${SCRIPT}" tinyllama

# Wrong checksum on existing file must fail (verify path, not silent skip).
export EXPECTED_SHA256="0000000000000000000000000000000000000000000000000000000000000000"
if bash "${SCRIPT}" tinyllama; then
	echo "test_download_model: expected SHA256 mismatch to fail" >&2
	exit 1
fi

# Non-empty file without EXPECTED_SHA256 skips verify and succeeds.
unset EXPECTED_SHA256
bash "${SCRIPT}" tinyllama

# Unknown model key.
if bash "${SCRIPT}" not-a-model; then
	echo "test_download_model: expected unknown model key to fail" >&2
	exit 1
fi

# Stub downloader: copy fixture via DOWNLOAD_CMD (no network).
stub_dl="${sandbox}/stub-download.sh"
cat >"${stub_dl}" <<STUB
#!/usr/bin/env bash
set -euo pipefail
cp "${FIXTURE}" "\$2"
STUB
chmod +x "${stub_dl}"

export EXPECTED_SHA256="${FIXTURE_SHA}"
unset SKIP_DOWNLOAD
rm -f "${dest}"
export DOWNLOAD_CMD="${stub_dl}"

bash "${SCRIPT}" tinyllama
test -s "${dest}"
bash "${SCRIPT}" tinyllama
unset DOWNLOAD_CMD

echo "test_download_model: OK"
