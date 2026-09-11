#!/usr/bin/env bash
# Validate ASAP Manifest JSON against upstream asap-protocol Pydantic models.
# Usage: manifest_build_json output | ./scripts/validate_manifest.sh
#        ./scripts/validate_manifest.sh path/to/manifest.json
# Exits 0 on success or graceful skip when the asap package is not installed (CI-safe).
set -euo pipefail

json=""
if [[ $# -ge 1 && -f "$1" ]]; then
	json="$(cat "$1")"
else
	json="$(cat)"
fi

if [[ -z "${json//[[:space:]]/}" ]]; then
	echo "validate_manifest: no JSON on stdin or file" >&2
	exit 1
fi

printf '%s' "$json" | python3 - <<'PY'
import json
import sys

raw = sys.stdin.read()
try:
    from asap.models.entities import Manifest
except ImportError:
    print("validate_manifest: SKIP (asap package not installed)")
    sys.exit(0)

Manifest.model_validate_json(raw)
print("validate_manifest: OK")
PY
