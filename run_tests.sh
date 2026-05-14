#!/usr/bin/env bash
# tests/run_tests.sh – diskdiag test suite
#
# Requires root (for loop devices and O_DIRECT).
# Run: sudo bash tests/run_tests.sh
#
# Options:
#   --no-device   Skip tests that require a loop device (for macOS CI)
#
# Exit code: 0 = all passed, 1 = one or more failed

set -euo pipefail

DISKDIAG=${DISKDIAG:-./diskdiag}
IMG=/tmp/diskdiag_test.img
LOOP=""
PASS=0
FAIL=0
NO_DEVICE=0

for arg in "$@"; do
    [[ "$arg" == "--no-device" ]] && NO_DEVICE=1
done

# ── Helpers ─────────────────────────────────────────────────────────

RED='\033[31m'
GREEN='\033[32m'
CYAN='\033[36m'
BOLD='\033[1m'
RESET='\033[0m'

info()  { echo -e "${CYAN}${BOLD}  $*${RESET}"; }
ok()    { echo -e "  ${GREEN}PASS${RESET}  $*"; PASS=$((PASS + 1)); }
fail()  { echo -e "  ${RED}FAIL${RESET}  $*"; FAIL=$((FAIL + 1)); }

# Run diskdiag, capture stdout+stderr, return its exit code
run() { "$DISKDIAG" "$@" 2>&1 || true; }
run_exit() { "$DISKDIAG" "$@" >/dev/null 2>&1; echo $?; }

assert_exit() {
    local desc="$1" expected="$2"; shift 2
    local got
    got=$(run_exit "$@")
    if [[ "$got" == "$expected" ]]; then
        ok "$desc (exit $got)"
    else
        fail "$desc – expected exit $expected, got $got"
    fi
}

assert_contains() {
    local desc="$1" pattern="$2"; shift 2
    local out
    out=$(run "$@")
    if echo "$out" | grep -qE "$pattern"; then
        ok "$desc"
    else
        fail "$desc – pattern '$pattern' not found in output"
        echo "    Output was: $out" | head -5
    fi
}

assert_not_contains() {
    local desc="$1" pattern="$2"; shift 2
    local out
    out=$(run "$@")
    if ! echo "$out" | grep -qE "$pattern"; then
        ok "$desc"
    else
        fail "$desc – pattern '$pattern' unexpectedly found"
    fi
}

assert_valid_json() {
    local desc="$1"; shift
    local out
    out=$(run "$@")
    if echo "$out" | python3 -m json.tool >/dev/null 2>&1; then
        ok "$desc"
    else
        fail "$desc – output is not valid JSON"
        echo "    Output: $out" | head -10
    fi
}

# ── Setup ────────────────────────────────────────────────────────────

setup() {
    info "Setup: creating 32 MiB test image and loop device"
    dd if=/dev/urandom bs=1M count=32 of="$IMG" 2>/dev/null
    LOOP=$(losetup --find --show "$IMG")
    info "Loop device: $LOOP"
    trap teardown EXIT
}

teardown() {
    [[ -n "$LOOP" ]] && losetup -d "$LOOP" 2>/dev/null || true
    rm -f "$IMG"
}

# ── Tests ────────────────────────────────────────────────────────────

test_build() {
    info "── Build"
    if [[ -x "$DISKDIAG" ]]; then
        ok "Binary exists and is executable"
    else
        fail "Binary '$DISKDIAG' not found – run make first"
        exit 1
    fi
}

test_help() {
    info "── Help and usage"
    assert_contains "help flag"          "Usage:"         --help
    assert_exit     "help exit code"     0                --help
    assert_exit     "no args exit code"  4
    assert_contains "no args message"    "no device"
}

test_bad_args() {
    info "── Argument validation"
    # These all fail before the device is opened, so /dev/null is fine as placeholder
    local dev=/dev/null
    assert_exit "bad block size (text)"    4 -b abc    "$dev"
    assert_exit "bad block size (zero)"    4 -b 0      "$dev"
    assert_exit "bad block size (too big)" 4 -b 9999   "$dev"
    assert_exit "bad block size (neg)"     4 -b -1     "$dev"
    assert_exit "bad block count"          4 -n 0      "$dev"
    assert_exit "bad offset (neg)"         4 -o -1     "$dev"
    assert_exit "warn >= critical"         4 \
        --threshold-warn 100 --threshold-critical 50 "$dev"
    assert_exit "threshold inf"            4 \
        --threshold-warn inf "$dev"
    assert_exit "threshold nan"            4 \
        --threshold-warn nan "$dev"
}

test_basic_read() {
    info "── Basic read"
    assert_exit "healthy exit code"  0 -y -q -n 16 "$LOOP"
    assert_contains "shows throughput"    "MiB/s"  -y -q -n 16 "$LOOP"
    assert_contains "shows latency min"   "Min"    -y -q -n 16 "$LOOP"
    assert_contains "shows latency p99"   "P99"    -y -q -n 16 "$LOOP"
    assert_contains "shows health"        "HEALTH" -y -q -n 16 "$LOOP"
}

test_block_sizes() {
    info "── Block sizes"
    assert_exit "1 MiB blocks"  0 -y -q -b 1 -n 8 "$LOOP"
    assert_exit "4 MiB blocks"  0 -y -q -b 4 -n 4 "$LOOP"
}

test_offset() {
    info "── Offset"
    assert_exit "offset + blocks"         0 -y -q -o 4 -n 4 "$LOOP"
    assert_exit "offset beyond device"    4 -o 999999        "$LOOP"
}

test_quiet() {
    info "── Quiet mode"
    assert_not_contains "no heatmap in quiet" "Heatmap" -y -q -n 8 "$LOOP"
    assert_contains     "stats in quiet"      "Results" -y -q -n 8 "$LOOP"
}

test_no_color() {
    info "── No-color mode"
    assert_not_contains "no ANSI codes" $'\033\[' \
        -y -q --no-color -n 8 "$LOOP"
}

test_json() {
    info "── JSON output"
    assert_valid_json "valid JSON"         -y -j -n 16 "$LOOP"
    assert_contains   "json has health"    '"health"'  -y -j -n 16 "$LOOP"
    assert_contains   "json has device"    '"device"'  -y -j -n 16 "$LOOP"
    assert_contains   "json has latency"   '"latency_ms_per_mib"' -y -j -n 16 "$LOOP"
    assert_contains   "json has exit_code" '"exit_code"'  -y -j -n 16 "$LOOP"
    assert_contains   "json has io_errors" '"io_errors"'  -y -j -n 16 "$LOOP"
    assert_contains   "json has critical"  '"critical_blocks"' -y -j -n 16 "$LOOP"
}

test_json_escape() {
    info "── JSON device name escaping"
    # Symlink with a quote in the name isn't easily testable,
    # but we verify the device field is always a valid JSON string
    local out
    out=$("$DISKDIAG" -y -j -n 4 "$LOOP" 2>/dev/null)
    local device
    device=$(echo "$out" | python3 -c \
        "import sys,json; d=json.load(sys.stdin); print(d['device'])" 2>/dev/null)
    if [[ -n "$device" ]]; then
        ok "device field is valid JSON string: $device"
    else
        fail "device field missing or not parseable"
    fi
}

test_thresholds() {
    info "── Custom thresholds"
    # With very tight warn threshold every block will be slow → FAIR or worse
    local exit_code
    exit_code=$(run_exit -y -q -n 16 \
        --threshold-warn 0.001 --threshold-critical 0.01 "$LOOP")
    if [[ "$exit_code" -ge 1 ]]; then
        ok "tight thresholds degrade health rating (exit $exit_code)"
    else
        fail "tight thresholds should not return HEALTHY"
    fi
}

test_pipe_suppresses_progress() {
    info "── Pipe suppresses progress bar"
    # When piped, \r-based progress should not appear
    local out
    out=$("$DISKDIAG" -y -n 8 "$LOOP" 2>/dev/null | cat)
    if ! echo "$out" | grep -qP '\r'; then
        ok "no carriage returns in piped output"
    else
        fail "carriage return found in piped output"
    fi
}

# ── Main ─────────────────────────────────────────────────────────────

echo -e "\n${BOLD}${CYAN}diskdiag test suite${RESET}\n"

test_build
test_help
test_bad_args   # these don't open a device (fail before open)

if [[ $NO_DEVICE -eq 1 ]]; then
    echo -e "  ${CYAN}Skipping device tests (--no-device)${RESET}"
else
    setup
    test_basic_read
    test_block_sizes
    test_offset
    test_quiet
    test_no_color
    test_json
    test_json_escape
    test_thresholds
    test_pipe_suppresses_progress
fi

echo ""
if [[ $FAIL -eq 0 ]]; then
    echo -e "${BOLD}${GREEN}All $PASS tests passed.${RESET}\n"
    exit 0
else
    echo -e "${BOLD}${RED}$FAIL of $((PASS + FAIL)) tests failed.${RESET}\n"
    exit 1
fi
