#!/usr/bin/env bash
#
# Federation test: two Go instances on this machine, the real C client.
#
#   cd client && make && cd ..
#   bash tests/federation_test.sh
#
# Covers a relay in each direction, a relay to an instance that is down
# (the client keeps a draft and sends it once the instance is back), and
# a block that the recipient's instance enforces on relayed mail.
#
# Requires: go, curl, a built client (client/bin/shyake). Ports 8791 and
# 8792 must be free.

set -u

ROOT=$(cd "$(dirname "$0")/.." && pwd)
CLI=$ROOT/client/bin/shyake
WORK=$(mktemp -d)
BIN=$WORK/shyake-server
A=127.0.0.1:8791
B=127.0.0.1:8792

export NO_PROXY='*' no_proxy='*' SHYAKE_PASSPHRASE=fedtest123

GREEN='\033[0;32m'
RED='\033[0;31m'
NC='\033[0m'
PASS=0
FAIL=0

ok() { echo -e "  ${GREEN}PASS${NC}  $1"; PASS=$((PASS + 1)); }
bad() { echo -e "  ${RED}FAIL${NC}  $1"; FAIL=$((FAIL + 1)); }
check() {
    if echo "$2" | grep -qF -- "$3"; then ok "$1"; else bad "$1: $2"; fi
}
section() { echo; echo "== $1"; }

cleanup() {
    for n in A B; do
        [ -f "$WORK/$n.pid" ] && kill "$(cat "$WORK/$n.pid")" 2>/dev/null
    done
    wait 2>/dev/null
    rm -rf "$WORK"
}
trap cleanup EXIT

# start NAME ADDR: run an instance with federation over plain HTTP
start() {
    SHYAKE_INSTANCE_DOMAIN=$2 SHYAKE_LISTEN=$2 \
    SHYAKE_DATABASE=$WORK/$1.db SHYAKE_FEDERATION_INSECURE=true \
        "$BIN" >>"$WORK/$1.log" 2>&1 &
    echo $! >"$WORK/$1.pid"
    for _ in $(seq 50); do
        curl -sf "http://$2/health" >/dev/null && return
        sleep 0.1
    done
    echo "instance $1 did not start:"; cat "$WORK/$1.log"; exit 1
}

stop() {
    kill "$(cat "$WORK/$1.pid")" 2>/dev/null
    wait "$(cat "$WORK/$1.pid")" 2>/dev/null
}

# profile DIR ADDR USER: create and register a client profile
profile() {
    mkdir -p "$1"
    printf '\n' | "$CLI" -c "$1" init >/dev/null 2>&1
    sed -i.bak "s|^INSTANCE=.*|INSTANCE=http://$2|; s|^USERNAME=.*|USERNAME=$3|" "$1/config"
    "$CLI" -c "$1" register -u "$3" >/dev/null 2>&1 || { echo "register $3 failed"; exit 1; }
}

[ -x "$CLI" ] || { echo "build the client first: cd client && make"; exit 1; }
(cd "$ROOT/server/go" && go build -o "$BIN" ./cmd/shyake-server) || exit 1

start A "$A"
start B "$B"
profile "$WORK/alice" "$A" alice
profile "$WORK/bobby" "$B" bobby

section "1. relay from A to B"
out=$(echo "hello across instances" |
    "$CLI" -c "$WORK/alice" send -t "bobby@$B" -s "fed-1" 2>&1)
check "alice sends to bobby@B" "$out" "Your mail was sent."
sleep 1
out=$("$CLI" -c "$WORK/bobby" --plain check inbox 2>&1)
check "bobby's inbox on B lists the mail" "$out" "fed-1"
id=$("$CLI" -c "$WORK/bobby" --plain check inbox 2>/dev/null | awk '/fed-1/{print $1}')
out=$("$CLI" -c "$WORK/bobby" --plain fetch "$id" 2>&1)
check "bobby decrypts the body" "$out" "hello across instances"
check "FROM shows the full address" "$out" "FROM:  alice@$A"
out=$("$CLI" -c "$WORK/alice" --plain check sent 2>&1)
check "alice's sent box on A keeps a copy" "$out" "fed-1"

section "2. reply from B to A"
out=$(echo "reply" | "$CLI" -c "$WORK/bobby" send -t "alice@$A" -s "fed-2" 2>&1)
check "bobby replies" "$out" "Your mail was sent."
sleep 1
out=$("$CLI" -c "$WORK/alice" --plain check inbox 2>&1)
check "alice receives the reply" "$out" "fed-2"

section "3. relay to an instance that is down"
# A caches bobby's key for a minute: send once to fill the cache, so the
# next send passes A's checks and fails at the relay itself
echo "warm" | "$CLI" -c "$WORK/alice" send -t "bobby@$B" -s "fed-3a" >/dev/null 2>&1
stop B
out=$(echo "sent while B was down" |
    "$CLI" -c "$WORK/alice" send -t "bobby@$B" -s "fed-3b" 2>&1)
check "A reports the failed relay" "$out" "unreachable"
check "the client keeps a draft" "$out" "Saved as draft"
out=$("$CLI" -c "$WORK/alice" --plain check sent 2>&1)
if echo "$out" | grep -qF "fed-3b"; then
    bad "A kept a sent copy of a mail it did not deliver"
else
    ok "A keeps no sent copy of the failed relay"
fi
draft=$("$CLI" -c "$WORK/alice" --plain check drafts 2>/dev/null |
    awk '/fed-3b/{print $1}')
start B "$B"
out=$("$CLI" -c "$WORK/alice" send -d "$draft" 2>&1)
check "the draft is sent once B is back" "$out" "Your mail was sent."
out=$("$CLI" -c "$WORK/bobby" --plain check inbox 2>&1)
check "the mail reaches B" "$out" "fed-3b"

section "4. block enforced by the recipient's instance"
out=$("$CLI" -c "$WORK/bobby" block "alice@$A" 2>&1)
check "bobby blocks alice@A" "$out" "blocked"
out=$(echo "blocked?" | "$CLI" -c "$WORK/alice" send -t "bobby@$B" -s "fed-4" 2>&1)
check "A passes B's refusal to the client" "$out" "blocked this sender"
out=$("$CLI" -c "$WORK/bobby" --plain check inbox 2>&1)
if echo "$out" | grep -qF "fed-4"; then
    bad "blocked mail was delivered"
else
    ok "blocked mail stays out of bobby's inbox"
fi

echo
echo "=========================================="
echo -e " Results: ${GREEN}$PASS passed${NC}, ${RED}$FAIL failed${NC} / $((PASS + FAIL)) total"
echo "=========================================="
[ "$FAIL" -eq 0 ]
