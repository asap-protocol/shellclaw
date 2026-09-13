#!/bin/sh
# Capture lcov per test and merge. Unit tests build with GATEWAY=0 (see Makefile coverage)
# so gcno/gcda stay scoped; test_gateway_http merges only its gateway slice so shellclaw
# subprocess coverage does not inflate the trace with zero-hit channel/tool objects.
set -e
ROOT="$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
BINDIR="${BINDIR:-build}"
COVERAGE_DIR="${COVERAGE_DIR:-build/coverage}"
COVERAGE_MIN="${COVERAGE_MIN:-80}"
LCOV_RC="lcov_branch_coverage=0"

mkdir -p "$COVERAGE_DIR"
rm -f "$COVERAGE_DIR"/*.info

TESTS="test_config test_memory test_skill test_provider test_anthropic test_openai test_local_provider test_router test_heartbeat test_agent test_reload test_channel test_cli test_shell test_file test_telegram test_discord_helpers test_web_search test_cron test_context test_dispatch test_crypto test_hardware_stub test_board_detect test_hardware_libgpiod test_hardware_i2c test_hardware_camera test_pin_tables test_hardware_init test_hardware_tools test_registry test_ws test_manifest_build test_manifest_keys test_jcs test_asap_envelope test_asap_ulid test_asap_client test_asap_registry test_asap_server test_asap_invoke test_asap_log test_sandbox test_allowlist test_rate_limit test_auth test_static"
# ASAP tests must stay aligned with ASAP_UNIT_TESTS in the top-level Makefile.
if [ "${GATEWAY:-}" = "1" ]; then
	TESTS="$TESTS test_gateway_http"
fi

capture_info() {
	name="$1"
	info="$COVERAGE_DIR/${name}.info"
	if lcov --capture --directory src --base-directory "$ROOT" --output-file "$info" --rc "$LCOV_RC" 2>/dev/null; then
		return 0
	fi
	lcov --capture --directory . --base-directory "$ROOT" --output-file "$info" --rc "$LCOV_RC" 2>/dev/null || true
}

merge_info() {
	info="$1"
	all="$2"
	if [ ! -s "$info" ]; then
		echo "coverage: skip empty $(basename "$info")"
		return 0
	fi
	if [ ! -s "$all" ]; then
		cp "$info" "$all"
		return 0
	fi
	if lcov -a "$all" -a "$info" -o "${all}.tmp" --rc "$LCOV_RC" 2>/dev/null; then
		mv "${all}.tmp" "$all"
	else
		rm -f "${all}.tmp"
		echo "coverage: warning: failed to merge $(basename "$info") (skipping)"
	fi
}

ALL_INFO="$COVERAGE_DIR/all.info"
for t in $TESTS; do
	find . -name '*.gcda' -delete 2>/dev/null || true
	case "$t" in
	test_static)
		if [ "${GATEWAY:-}" = "1" ]; then
			${MAKE:-make} BUILD=coverage GATEWAY=1 src/gateway/static.o >/dev/null 2>&1 || true
		fi
		exe="$BINDIR/$t"
		if [ ! -f "$exe" ]; then
			echo "coverage: skip $t (not built)"
			continue
		fi
		if ! "$exe"; then
			echo "coverage: $t failed"
			exit 1
		fi
		capture_info "$t"
		merge_info "$COVERAGE_DIR/${t}.info" "$ALL_INFO"
		;;
	test_gateway_http)
		exe="$BINDIR/$t"
		if [ ! -f "$exe" ]; then
			echo "coverage: skip $t (not built)"
			continue
		fi
		if ! "$exe"; then
			echo "coverage: $t failed"
			exit 1
		fi
		capture_info "$t"
		gw_slice="$COVERAGE_DIR/${t}_gateway.info"
		if lcov --extract "$COVERAGE_DIR/${t}.info" \
			'*/gateway/auth.c' '*/gateway/http.c' '*/gateway/rate_limit.c' \
			'*/gateway/ws.c' '*/gateway/static.c' \
			--output-file "$gw_slice" --rc "$LCOV_RC" --ignore-errors unused 2>/dev/null; then
			merge_info "$gw_slice" "$ALL_INFO"
		else
			echo "coverage: warning: no gateway slice from $t"
		fi
		;;
	*)
		exe="$BINDIR/$t"
		if [ ! -f "$exe" ]; then
			echo "coverage: skip $t (not built)"
			continue
		fi
		if ! "$exe"; then
			echo "coverage: $t failed"
			exit 1
		fi
		capture_info "$t"
		merge_info "$COVERAGE_DIR/${t}.info" "$ALL_INFO"
		;;
	esac
done

if [ ! -s "$ALL_INFO" ]; then
	echo "Could not collect coverage data"
	exit 1
fi

CORE_INFO="$COVERAGE_DIR/core.info"
# Phase 4 integration-only (shellclaw + test_gateway_http): not unit-isolated.
# Phase 5 hardware/: validated by test_hardware_* (Mac + Linux CI), not the 80% agent-core gate.
lcov --remove "$ALL_INFO" '/usr/*' 'vendor/*' 'tests/*' '*/channels/*' '*/tools/*' '*/providers/*' '*/core/main.c' \
	'*/hardware/*' \
	'*/gateway/http_lws.c' '*/gateway/routes.c' '*/core/bootstrap.c' '*/core/daemon.c' '*/core/dispatch.c' \
	--output-file "$CORE_INFO" --rc "$LCOV_RC" --ignore-errors unused 2>/dev/null || true

pct=$(lcov --summary "$CORE_INFO" 2>/dev/null | grep 'lines' | grep -oE '[0-9]+\.?[0-9]*' | head -1)
if [ -n "$pct" ]; then
	if ! awk "BEGIN { exit !($pct >= $COVERAGE_MIN) }"; then
		echo "Coverage ${pct}% is below ${COVERAGE_MIN}%"
		lcov --list "$CORE_INFO" 2>/dev/null || true
		exit 1
	fi
	echo "Coverage: ${pct}% (>= ${COVERAGE_MIN}%)"
else
	echo "Could not parse coverage"
	exit 1
fi
genhtml "$CORE_INFO" -o "$COVERAGE_DIR/html" --quiet 2>/dev/null || true
