#!/usr/bin/env bash
# Verify a dumped ASAP SignedManifest with the upstream reference verifier
# asap.crypto.signing.verify_manifest (the one docs/ASAP.md tells operators to
# use). This is the end-to-end SIG gate: it proves the C-side JCS + Ed25519
# signing produces bytes the third-party upstream verifier accepts.
#
# Usage:
#   ./scripts/verify_manifest.sh path/to/signed_manifest.json
#   ./scripts/dump_manifest.sh | ./scripts/verify_manifest.sh
#
# Exit codes: 0 = verified OK (or graceful SKIP when asap-protocol is absent,
#             unless --strict); 1 = usage/no-JSON/strict-mode missing package;
#             2 = SignedManifest shape invalid; 3 = upstream rejected the
#             signature (verification failed).
#
# Requires Python >= 3.13 with `asap-protocol` installed (pip install
# asap-protocol). When the package is missing, exits 0 with a SKIP notice so
# this script is safe to wire into CI that may not have the package yet. Pass
# --strict to instead fail (exit 1) when the package is absent -- use that when
# wiring into a release gate that must guarantee the verifier is present.
set -euo pipefail

strict=0
json=""
while [[ $# -gt 0 ]]; do
	case "$1" in
	--strict)
		strict=1
		shift
		;;
	-*)
		echo "verify_manifest: unknown option: $1" >&2
		exit 1
		;;
	*)
		if [[ -f "$1" ]]; then
			json="$(cat "$1")"
		else
			echo "verify_manifest: file not found: $1" >&2
			exit 1
		fi
		shift
		;;
	esac
done
if [[ -z "$json" ]]; then
	json="$(cat)"
fi

if [[ -z "${json//[[:space:]]/}" ]]; then
	echo "verify_manifest: no JSON on stdin or file" >&2
	exit 1
fi

SC_STRICT="$strict" SC_SIGNED_MANIFEST="$json" python3 - <<'PY'
import json
import os
import sys

strict = os.environ.get("SC_STRICT", "0") == "1"
raw = os.environ.get("SC_SIGNED_MANIFEST", "")
if not raw:
    print("verify_manifest: empty SC_SIGNED_MANIFEST env", file=sys.stderr)
    sys.exit(1)
try:
    from asap.crypto.signing import verify_manifest
    from asap.crypto.models import SignedManifest
except ImportError:
    if strict:
        print("verify_manifest: FAIL (--strict: asap-protocol not installed; "
              "pip install asap-protocol)", file=sys.stderr)
        sys.exit(1)
    print("verify_manifest: SKIP (asap-protocol package not installed; "
          "pip install asap-protocol)")
    sys.exit(0)

try:
    data = json.loads(raw)
    sm = SignedManifest.model_validate(data)
except Exception as e:
    print(f"verify_manifest: invalid SignedManifest shape: {e}", file=sys.stderr)
    sys.exit(2)

try:
    ok = verify_manifest(sm)
except Exception as e:
    print(f"verify_manifest: upstream rejected: {e}", file=sys.stderr)
    sys.exit(3)

if not ok:
    print("verify_manifest: upstream rejected (verify_manifest returned False)",
          file=sys.stderr)
    sys.exit(3)

print(f"verify_manifest: OK (alg={sm.signature.alg}, "
      f"trust={sm.signature.trust_level})")
PY
