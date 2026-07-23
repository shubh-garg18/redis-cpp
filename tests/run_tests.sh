#!/usr/bin/env bash
# Integration tests. Boots the server on a scratch port + temp dir, fires
# real commands with redis-cli, and checks the replies match real Redis.
# No unit hooks — the server is a process you talk to over TCP, so we test it
# the way a client would. Run from anywhere: ./tests/run_tests.sh

set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$ROOT/build/redis_server"
PORT=6399
WORK="$(mktemp -d)"

pass=0
fail=0

if [ ! -x "$BIN" ]; then
    echo "build first: cmake -S . -B build && cmake --build build"
    exit 1
fi

SRV_PID=""
start_server() {              # args: extra flags for redis_server
    "$BIN" --port "$PORT" --dir "$WORK" "$@" >"$WORK/server.log" 2>&1 &
    SRV_PID=$!
}
stop_server() {
    if [ -n "$SRV_PID" ]; then
        kill "$SRV_PID" 2>/dev/null
        wait "$SRV_PID" 2>/dev/null
        SRV_PID=""
    fi
}
wait_ready() {
    # An auth-enabled server answers PING with NOAUTH, not PONG — both mean "up".
    for _ in $(seq 1 50); do
        case "$(redis-cli -p "$PORT" PING 2>/dev/null)" in
            PONG | *NOAUTH*) return 0 ;;
        esac
        sleep 0.1
    done
    echo "server never became ready; log:"; cat "$WORK/server.log"; exit 1
}

RSRV_PID=""
RWORK=""
cleanup() {
    stop_server
    [ -n "$RSRV_PID" ] && { kill "$RSRV_PID" 2>/dev/null; wait "$RSRV_PID" 2>/dev/null; }
    rm -rf "$WORK" "$RWORK"
}
trap cleanup EXIT INT TERM

rc() { redis-cli -p "$PORT" "$@" 2>&1; }

assert() {                    # desc, expected, actual
    if [ "$2" = "$3" ]; then
        pass=$((pass + 1)); printf '  ok   %s\n' "$1"
    else
        fail=$((fail + 1))
        printf 'FAIL   %s\n         expected: [%s]\n         got:      [%s]\n' "$1" "$2" "$3"
    fi
}
assert_has() {                # desc, needle, actual
    case "$3" in
        *"$2"*) pass=$((pass + 1)); printf '  ok   %s\n' "$1" ;;
        *) fail=$((fail + 1))
           printf 'FAIL   %s\n         wanted substring: [%s]\n         got:      [%s]\n' "$1" "$2" "$3" ;;
    esac
}

# ---------------------------------------------------------------- main server
start_server
wait_ready

echo "== strings =="
assert "PING"              "PONG"  "$(rc PING)"
assert "ECHO"              "hi"    "$(rc ECHO hi)"
assert "SET"               "OK"    "$(rc SET k v)"
assert "GET"               "v"     "$(rc GET k)"
assert "GET missing = nil" ""      "$(rc GET nope)"
assert "INCR fresh"        "1"     "$(rc INCR n)"
assert "INCR again"        "2"     "$(rc INCR n)"
assert_has "INCR on string errors" "ERR" "$(rc INCR k)"
assert "TYPE string"       "string" "$(rc TYPE k)"
assert "DEL"               "1"     "$(rc DEL k)"
assert "TYPE gone = none"  "none"  "$(rc TYPE k)"

echo "== expiry =="
rc SET tk tv PX 100000 >/dev/null
assert "SET PX then GET"   "tv"    "$(rc GET tk)"

echo "== lists =="
assert "RPUSH"             "3"     "$(rc RPUSH l a b c)"
assert "LLEN"              "3"     "$(rc LLEN l)"
assert "LRANGE all"        "$(printf 'a\nb\nc')" "$(rc LRANGE l 0 -1)"
assert "LPUSH"             "4"     "$(rc LPUSH l z)"
assert "LPOP"              "z"     "$(rc LPOP l)"
assert "BLPOP with data"   "$(printf 'l\na')"    "$(rc BLPOP l 0)"
assert "TYPE list"         "list"  "$(rc TYPE l)"

echo "== sorted sets =="
assert "ZADD"              "2"     "$(rc ZADD z 1 a 2 b)"
assert "ZSCORE"            "1"     "$(rc ZSCORE z a)"
assert "ZRANGE"            "$(printf 'a\nb')"     "$(rc ZRANGE z 0 -1)"
assert "ZCARD"             "2"     "$(rc ZCARD z)"
assert "ZRANK"             "1"     "$(rc ZRANK z b)"
assert "ZREM"             "1"     "$(rc ZREM z a)"
assert "ZCARD after ZREM"  "1"     "$(rc ZCARD z)"
assert "TYPE zset"         "zset"  "$(rc TYPE z)"

echo "== streams =="
assert "XADD explicit id"  "5-5"   "$(rc XADD s 5-5 field val)"
assert_has "XRANGE sees entry" "5-5" "$(rc XRANGE s - +)"
assert_has "XADD auto id"  "-"     "$(rc XADD s '*' k2 v2)"
assert "TYPE stream"       "stream" "$(rc TYPE s)"

echo "== geo =="
assert "GEOADD"            "1"     "$(rc GEOADD g 13.361389 38.115556 Palermo)"
rc GEOADD g 15.087269 37.502669 Catania >/dev/null
assert "GEODIST self = 0" "0.0000" "$(rc GEODIST g Palermo Palermo)"
assert_has "GEOPOS has lon" "13.36" "$(rc GEOPOS g Palermo)"
assert_has "GEOSEARCH finds Palermo" "Palermo" \
    "$(rc GEOSEARCH g FROMLONLAT 15 37 BYRADIUS 300 km ASC)"

echo "== transactions =="
# MULTI needs several commands on one connection, so drive it through stdin.
printf 'MULTI\nSET t1 hello\nINCR t2\nEXEC\n' | redis-cli -p "$PORT" >/dev/null 2>&1
assert "MULTI/EXEC committed SET"  "hello" "$(rc GET t1)"
assert "MULTI/EXEC committed INCR" "1"     "$(rc GET t2)"
printf 'SET d1 keep\nMULTI\nSET d1 changed\nDISCARD\n' | redis-cli -p "$PORT" >/dev/null 2>&1
assert "DISCARD dropped the write" "keep"  "$(rc GET d1)"

echo "== pub/sub (observable-in-serial) =="
assert "PUBSUB CHANNELS empty"     ""  "$(rc PUBSUB CHANNELS)"
assert "PUBLISH no subscribers = 0" "0" "$(rc PUBLISH news hi)"

echo "== wrong type =="
assert_has "GET on a list is WRONGTYPE" "WRONGTYPE" "$(rc GET l)"

stop_server

# ---------------------------------------------------------------- auth server
echo "== auth (separate --requirepass server) =="
start_server --requirepass s3cret
wait_ready
assert_has "no auth = NOAUTH"     "NOAUTH"   "$(rc GET x)"
assert_has "wrong pass = WRONGPASS" "WRONGPASS" "$(rc AUTH nope)"
assert "AUTH then command"        "OK"       "$(printf 'AUTH s3cret\nSET a 1\n' | redis-cli -p "$PORT" 2>&1 | tail -1)"
assert "ACL WHOAMI"               "default"  "$(redis-cli -p "$PORT" -a s3cret ACL WHOAMI 2>/dev/null)"
stop_server

# ---------------------------------------------------------------- aof restart
echo "== aof (survives restart) =="
start_server --appendonly yes
wait_ready
rc SET durable yes >/dev/null
rc RPUSH dlist a b >/dev/null
stop_server
start_server --appendonly yes
wait_ready
assert "AOF replayed string"  "yes"  "$(rc GET durable)"
assert "AOF replayed list"    "2"    "$(rc LLEN dlist)"
stop_server

# ---------------------------------------------------------------- lru eviction
echo "== lru eviction (--maxkeys 3) =="
start_server --maxkeys 3
wait_ready
rc SET e1 1 >/dev/null; rc SET e2 1 >/dev/null; rc SET e3 1 >/dev/null
rc GET e1 >/dev/null                # refresh e1 so e2 becomes oldest
rc SET e4 1 >/dev/null              # 4th key -> evict oldest (e2)
assert "refreshed key survives"    "1"  "$(rc GET e1)"
assert "oldest key evicted"        ""   "$(rc GET e2)"
assert "newest key present"        "1"  "$(rc GET e4)"
assert "count stays at cap"        "3"  "$(rc KEYS '*' | grep -c .)"
stop_server

# ---------------------------------------------------------------- replication
echo "== replication (master + replica) =="
RPORT=$((PORT + 1))
RWORK="$(mktemp -d)"
start_server                                    # master on $PORT
wait_ready
rc SET rep_pre v1 >/dev/null                     # preload -> exercises the snapshot
"$BIN" --port "$RPORT" --dir "$RWORK" --replicaof 127.0.0.1 "$PORT" >"$RWORK/server.log" 2>&1 &
RSRV_PID=$!
for _ in $(seq 1 50); do
    [ "$(redis-cli -p "$RPORT" PING 2>/dev/null)" = "PONG" ] && break
    sleep 0.1
done
sleep 0.5                                        # let the sync land
assert "snapshot reached replica" "v1" "$(redis-cli -p "$RPORT" GET rep_pre 2>/dev/null)"
rc SET rep_live v2 >/dev/null
sleep 0.3
assert "live write propagated"    "v2" "$(redis-cli -p "$RPORT" GET rep_live 2>/dev/null)"
assert_has "replica is read-only"  "READONLY"          "$(redis-cli -p "$RPORT" SET x 1 2>/dev/null)"
assert_has "replica INFO role"     "role:slave"        "$(redis-cli -p "$RPORT" INFO replication 2>/dev/null | tr -d '\r')"
assert_has "master sees replica"   "connected_slaves:1" "$(redis-cli -p "$PORT" INFO replication 2>/dev/null | tr -d '\r')"
kill "$RSRV_PID" 2>/dev/null; wait "$RSRV_PID" 2>/dev/null; RSRV_PID=""
rm -rf "$RWORK"; RWORK=""
stop_server

# ---------------------------------------------------------------- summary
echo
echo "-----------------------------------------"
printf '%d passed, %d failed\n' "$pass" "$fail"
[ "$fail" -eq 0 ]
