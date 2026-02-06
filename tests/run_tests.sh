#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"
JSB="$ROOT_DIR/jsb"
TEST_SERVER="$SCRIPT_DIR/test_server"
PORT=18080
PASS=0
FAIL=0
ERRORS=""

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

cleanup() {
    if [ -n "$SERVER_PID" ]; then
        kill "$SERVER_PID" 2>/dev/null || true
        wait "$SERVER_PID" 2>/dev/null || true
    fi
}
trap cleanup EXIT

# Start test server
echo -e "${YELLOW}Starting test server on port $PORT...${NC}"
"$TEST_SERVER" "$PORT" &
SERVER_PID=$!
sleep 0.5

# Check server is running
if ! kill -0 "$SERVER_PID" 2>/dev/null; then
    echo -e "${RED}FATAL: Test server failed to start${NC}"
    exit 1
fi

run_cli_test() {
    local name="$1"
    local script="$2"
    printf "  %-35s " "$name"

    output=$("$JSB" "$script" 2>&1)
    exit_code=$?

    if [ $exit_code -eq 0 ] && echo "$output" | grep -q "PASS:"; then
        echo -e "${GREEN}PASS${NC}"
        PASS=$((PASS + 1))
    else
        echo -e "${RED}FAIL${NC}"
        FAIL=$((FAIL + 1))
        ERRORS="$ERRORS\n  $name: exit=$exit_code output=$(echo "$output" | tail -3)"
    fi
}

run_bench_test() {
    local name="$1"
    local script="$2"
    printf "  %-35s " "$name"

    output=$("$JSB" "$script" 2>&1)
    exit_code=$?

    # Check that it ran successfully and produced stats
    if [ $exit_code -eq 0 ] && echo "$output" | grep -q "requests:"; then
        # Extract request count
        reqs=$(echo "$output" | grep "requests:" | awk '{print $2}')
        errors=$(echo "$output" | grep "errors:" | awk '{print $2}')
        if [ "$reqs" -gt 0 ] && [ "$errors" = "0" ]; then
            echo -e "${GREEN}PASS${NC} (${reqs} reqs, 0 errors)"
            PASS=$((PASS + 1))
        else
            echo -e "${RED}FAIL${NC} (${reqs} reqs, ${errors} errors)"
            FAIL=$((FAIL + 1))
            ERRORS="$ERRORS\n  $name: reqs=$reqs errors=$errors"
        fi
    else
        echo -e "${RED}FAIL${NC}"
        FAIL=$((FAIL + 1))
        ERRORS="$ERRORS\n  $name: exit=$exit_code output=$(echo "$output" | tail -3)"
    fi
}

echo ""
echo -e "${YELLOW}=== CLI Mode Tests ===${NC}"
run_cli_test "GET request"           "$SCRIPT_DIR/scripts/test_get.js"
run_cli_test "POST with body"        "$SCRIPT_DIR/scripts/test_post.js"
run_cli_test "Request headers"       "$SCRIPT_DIR/scripts/test_headers.js"
run_cli_test "JSON parsing"          "$SCRIPT_DIR/scripts/test_json.js"
run_cli_test "HTTP status codes"     "$SCRIPT_DIR/scripts/test_status.js"
run_cli_test "Sequential fetches"    "$SCRIPT_DIR/scripts/test_multi_fetch.js"
run_cli_test "Concurrent fetches"    "$SCRIPT_DIR/scripts/test_concurrent.js"
run_cli_test "Headers API"           "$SCRIPT_DIR/scripts/test_headers_api.js"
run_cli_test "Chunked encoding"      "$SCRIPT_DIR/scripts/test_chunked.js"

echo ""
echo -e "${YELLOW}=== Benchmark Mode Tests ===${NC}"
run_bench_test "String export"       "$SCRIPT_DIR/scripts/bench_string.js"
run_bench_test "Object export"       "$SCRIPT_DIR/scripts/bench_object.js"
run_bench_test "Array round-robin"   "$SCRIPT_DIR/scripts/bench_array.js"
run_bench_test "Async function"      "$SCRIPT_DIR/scripts/bench_async.js"
run_bench_test "Options (conns/thr)" "$SCRIPT_DIR/scripts/bench_options.js"

echo ""
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
TOTAL=$((PASS + FAIL))
echo -e "Results: ${GREEN}$PASS passed${NC}, ${RED}$FAIL failed${NC}, $TOTAL total"

if [ $FAIL -gt 0 ]; then
    echo -e "\n${RED}Failures:${NC}"
    echo -e "$ERRORS"
    echo ""
    exit 1
fi

echo ""
exit 0
