#!/bin/bash
# Usage: run_with_mock.sh <test-binary> [port]. Starts the Node mock on a private port, runs the
# binary with MOCK_PORT set, then stops only the mock it started.
BIN="$1"; PORT="${2:-$((20000 + $$ % 20000))}"
DIR="$(cd "$(dirname "$0")/.." && pwd)"
node "$DIR/sim/mock/domoticz_mock.js" --port "$PORT" >/dev/null 2>&1 &
MP=$!
trap 'kill $MP 2>/dev/null; wait $MP 2>/dev/null' EXIT INT TERM
i=0
while [ $i -lt 50 ]; do
  (exec 3<>/dev/tcp/127.0.0.1/"$PORT") 2>/dev/null && break
  i=$((i+1)); sleep 0.1
done
MOCK_PORT="$PORT" "$BIN"
