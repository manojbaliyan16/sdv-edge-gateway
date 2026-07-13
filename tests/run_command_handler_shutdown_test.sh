#!/usr/bin/env bash
# Orchestrates test_command_handler_shutdown: starts fake_broker.py, runs the
# test binary under a hard outer timeout (so a genuine infinite hang in
# CommandHandler::stop() fails the test instead of hanging CI/the shell
# forever), and reports pass/fail.
#
# Usage: run_command_handler_shutdown_test.sh <path-to-test-binary>
set -u

TEST_BIN="${1:?usage: run_command_handler_shutdown_test.sh <test-binary>}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PORT=18884
OUTER_TIMEOUT_S=15

TIMEOUT_BIN="timeout"
command -v timeout >/dev/null 2>&1 || TIMEOUT_BIN="gtimeout"
if ! command -v "$TIMEOUT_BIN" >/dev/null 2>&1; then
    echo "FAIL: need 'timeout' or 'gtimeout' on PATH (brew install coreutils)"
    exit 1
fi

pkill -9 -f "fake_broker.py $PORT" 2>/dev/null
sleep 0.3

BROKER_LOG="$(mktemp)"
python3 "$SCRIPT_DIR/fake_broker.py" "$PORT" > "$BROKER_LOG" 2>&1 &
BROKER_PID=$!
trap 'kill -9 "$BROKER_PID" 2>/dev/null; rm -f "$BROKER_LOG"' EXIT

for _ in $(seq 1 20); do
    grep -q listening "$BROKER_LOG" 2>/dev/null && break
    sleep 0.2
done

"$TIMEOUT_BIN" "$OUTER_TIMEOUT_S" "$TEST_BIN" "$PORT"
RESULT=$?

if [ "$RESULT" -eq 124 ]; then
    echo "FAIL: test binary hit the outer ${OUTER_TIMEOUT_S}s timeout -- CommandHandler::stop() hung"
    exit 1
elif [ "$RESULT" -ne 0 ]; then
    echo "FAIL: test binary exited $RESULT"
    exit "$RESULT"
fi

exit 0
