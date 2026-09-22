#!/usr/bin/env bash
# Mirrors .github/workflows/ci.yml (excluding optional asap-compliance pip install).
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"
export CI=true
export GATEWAY=1
if command -v apt-get >/dev/null 2>&1; then
	echo "==> apt-get (CI deps incl. libgpiod-dev >= 2.x, i2c-tools; use Ubuntu 24.04+)"
	sudo apt-get update
	sudo apt-get install -y build-essential libcurl4-openssl-dev libwebsockets-dev libgpiod-dev i2c-tools cppcheck lcov nodejs
fi
echo "==> cppcheck (make static)"
make static
echo "==> clean + test (CI=true, -Werror, libgpiod present)"
make clean && make test
echo "==> clean + test without libgpiod (stub GPIO fallback)"
make clean && LIBGPIOD=0 make test
echo "==> AddressSanitizer + UBSan (make test-sanitize)"
make test-sanitize
echo "==> release build"
make clean && make release
size=$(stat -f%z build/shellclaw 2>/dev/null || stat -c%s build/shellclaw)
max=$((2 * 1024 * 1024))
if [ "$size" -gt "$max" ]; then
	echo "Binary size $size bytes exceeds ${max} bytes"
	exit 1
fi
echo "Binary size: $size bytes (OK)"
echo "==> coverage"
make coverage
echo "==> ci-local: all steps passed"
