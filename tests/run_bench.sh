#!/usr/bin/env bash
# Throughput benchmark. Boots a scratch server and runs redis-benchmark against
# the commands this server actually implements, printing requests/sec per
# command. Use it to get a baseline now and a before/after when the write path
# changes (e.g. adding eviction). Run from anywhere: ./tests/run_bench.sh

set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$ROOT/build/redis_server"
PORT=6399
WORK="$(mktemp -d)"

# Fewer requests/clients than redis-benchmark's defaults so a run finishes fast;
# override from the environment if you want a heavier pass.
REQUESTS="${REQUESTS:-50000}"
CLIENTS="${CLIENTS:-25}"

# Built-in redis-benchmark tests whose commands this server supports and which
# run in O(1)/O(log n). Two exclusions on purpose:
#   ping         - redis-benchmark's PING test uses the inline protocol
#                  (PING\r\n); this server speaks RESP arrays only.
#   lpush, lpop  - lists are vector-backed, so front ops are O(n) (see
#                  docs/design-decisions.md §10). Under high volume they go
#                  quadratic and dominate the run. To see that cost on purpose:
#                  TESTS=lpush,lpop REQUESTS=5000 ./tests/run_bench.sh
TESTS="${TESTS:-set,get,incr,rpush,zadd}"

if [ ! -x "$BIN" ]; then
    echo "build first: cmake -S . -B build && cmake --build build"
    exit 1
fi
command -v redis-benchmark >/dev/null || { echo "redis-benchmark not installed"; exit 1; }

"$BIN" --port "$PORT" --dir "$WORK" >"$WORK/server.log" 2>&1 &
SRV_PID=$!
cleanup() { kill "$SRV_PID" 2>/dev/null; wait "$SRV_PID" 2>/dev/null; rm -rf "$WORK"; }
trap cleanup EXIT INT TERM

for _ in $(seq 1 50); do
    [ "$(redis-cli -p "$PORT" PING 2>/dev/null)" = "PONG" ] && break
    sleep 0.1
done

echo "redis_server  —  $REQUESTS requests, $CLIENTS clients"
echo "-----------------------------------------------------"
echo "serial (-P 1):"
redis-benchmark -p "$PORT" -n "$REQUESTS" -c "$CLIENTS" -t "$TESTS" -q
echo
echo "pipelined (-P ${PIPELINE:-16}):"
redis-benchmark -p "$PORT" -n "$REQUESTS" -c "$CLIENTS" -P "${PIPELINE:-16}" -t "$TESTS" -q
