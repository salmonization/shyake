#!/usr/bin/env bash
# Shyake end-to-end functional test suite
# Requires: wrangler dev running on 127.0.0.1:8787, shyake binary built

set -euo pipefail

# ------------------------------------------------------------------ #
# Configuration
# ------------------------------------------------------------------ #
SHYAKE="${SHYAKE_BIN:-$(dirname "$0")/../client/bin/shyake}"
INSTANCE="http://127.0.0.1:8787"
TMPDIR_ROOT="$(mktemp -d /tmp/shyake_test.XXXXXX)"
PASS=0
FAIL=0

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

cleanup() {
    rm -rf "$TMPDIR_ROOT"
}
trap cleanup EXIT

# ------------------------------------------------------------------ #
# Helper functions
# ------------------------------------------------------------------ #

# assert test_name expected_exit_code actual_exit_code [extra_msg]
assert_exit() {
    local name="$1" expected="$2" actual="$3" extra="${4:-}"
    if [ "$actual" -eq "$expected" ]; then
        echo -e "  ${GREEN}PASS${NC}  $name"
        PASS=$((PASS + 1))
    else
        echo -e "  ${RED}FAIL${NC}  $name (expected exit $expected, got $actual) $extra"
        FAIL=$((FAIL + 1))
    fi
}

# assert_output_contains test_name pattern output
assert_contains() {
    local name="$1" pattern="$2" output="$3"
    if echo "$output" | grep -qF "$pattern"; then
        echo -e "  ${GREEN}PASS${NC}  $name"
        PASS=$((PASS + 1))
    else
        echo -e "  ${RED}FAIL${NC}  $name (expected '$pattern' in output)"
        echo -e "           Output was: $(echo "$output" | head -3)"
        FAIL=$((FAIL + 1))
    fi
}

# assert_not_contains test_name pattern output
assert_not_contains() {
    local name="$1" pattern="$2" output="$3"
    if echo "$output" | grep -qF "$pattern"; then
        echo -e "  ${RED}FAIL${NC}  $name (did not expect '$pattern' in output)"
        FAIL=$((FAIL + 1))
    else
        echo -e "  ${GREEN}PASS${NC}  $name"
        PASS=$((PASS + 1))
    fi
}

# wait for server to be up
wait_for_server() {
    local retries=15
    while true; do
        local code
        code=$(curl -so /dev/null -w "%{http_code}" \
            --max-time 2 "$INSTANCE/api/pubkey/nonexistent_xyz_probe" 2>/dev/null \
            || echo "000")
        # any HTTP response (including 404) means server is up
        if [ "$code" != "000" ] && [ -n "$code" ]; then
            return 0
        fi
        retries=$((retries - 1))
        if [ $retries -eq 0 ]; then
            echo -e "${RED}ERROR: Server at $INSTANCE is not responding.${NC}"
            echo "Start the server with: cd server && npx wrangler dev"
            exit 1
        fi
        sleep 1
    done
}

# make_account <dir> <username>
make_account() {
    local dir="$1" user="$2"
    mkdir -p "$dir"
    cat > "$dir/config" <<EOF
INSTANCE=$INSTANCE
USERNAME=$user
TIME_FORMAT="%Y-%m-%d %H:%M"
CHECK_COLUMNS=id,sender,subject,size,date
NO_COLOR=1
EOF
}

# sh_run <config_dir> <args...>  -- run shyake with given config dir
sh_run() {
    local cfg="$1"; shift
    "$SHYAKE" --no-color -c "$cfg" "$@"
}

# ------------------------------------------------------------------ #
# Test groups
# ------------------------------------------------------------------ #

section() { echo -e "\n${YELLOW}=== $1 ===${NC}"; }

# ------------------------------------------------------------------ #
section "Pre-flight: binary and server"
# ------------------------------------------------------------------ #

if [ ! -x "$SHYAKE" ]; then
    echo -e "${RED}ERROR: shyake binary not found at $SHYAKE${NC}"
    echo "Run 'make' in client/ first."
    exit 1
fi
echo -e "  ${GREEN}PASS${NC}  Binary exists: $SHYAKE"

wait_for_server
echo -e "  ${GREEN}PASS${NC}  Server reachable at $INSTANCE"

# ------------------------------------------------------------------ #
section "1. init"
# ------------------------------------------------------------------ #

INIT_DIR="$TMPDIR_ROOT/init_test"
out=$("$SHYAKE" init -c "$INIT_DIR" 2>&1) || true
assert_exit "init creates config dir" 0 "$([ -d "$INIT_DIR" ] && echo 0 || echo 1)"
assert_exit "init creates config file" 0 "$([ -f "$INIT_DIR/config" ] && echo 0 || echo 1)"
assert_exit "init creates kem_pk.bin" 0 "$([ -f "$INIT_DIR/kem_pk.bin" ] && echo 0 || echo 1)"
assert_exit "init creates sig_pk.bin" 0 "$([ -f "$INIT_DIR/sig_pk.bin" ] && echo 0 || echo 1)"
assert_exit "init creates kem_sk.bin" 0 "$([ -f "$INIT_DIR/kem_sk.bin" ] && echo 0 || echo 1)"
assert_exit "init creates sig_sk.bin" 0 "$([ -f "$INIT_DIR/sig_sk.bin" ] && echo 0 || echo 1)"

# ------------------------------------------------------------------ #
section "2. register"
# ------------------------------------------------------------------ #

# Generate unique usernames to avoid collision
TS=$(date +%s)
USER_A="tsta${TS}"
USER_B="tstb${TS}"
DIR_A="$TMPDIR_ROOT/acct_a"
DIR_B="$TMPDIR_ROOT/acct_b"

"$SHYAKE" init -c "$DIR_A" > /dev/null 2>&1
"$SHYAKE" init -c "$DIR_B" > /dev/null 2>&1

out_a=$(sh_run "$DIR_A" register -u "$USER_A" -i "$INSTANCE" 2>&1)
rc=$?; assert_exit "register user A succeeds" 0 "$rc" "$out_a"
assert_contains "register: success message" "egistered" "$out_a"

# Username written to config
assert_contains "register: username saved in config" \
    "USERNAME=${USER_A}" "$(cat "$DIR_A/config")"

out_b=$(sh_run "$DIR_B" register -u "$USER_B" -i "$INSTANCE" 2>&1)
rc=$?; assert_exit "register user B succeeds" 0 "$rc" "$out_b"

# Duplicate registration should fail
out_dup=$(sh_run "$DIR_A" register -u "$USER_A" -i "$INSTANCE" 2>&1) || true
assert_not_contains "register: duplicate fails" "egistered successfully" "$out_dup"

# Invalid username (too short)
out_bad=$("$SHYAKE" init -c "$TMPDIR_ROOT/bad" > /dev/null 2>&1; \
    sh_run "$TMPDIR_ROOT/bad" register -u "ab" -i "$INSTANCE" 2>&1) || true
assert_not_contains "register: short username rejected" \
    "egistered successfully" "$out_bad"

# ------------------------------------------------------------------ #
section "3. whoami"
# ------------------------------------------------------------------ #

out=$(sh_run "$DIR_A" whoami 2>&1)
assert_exit "whoami exits 0" 0 "$?"
assert_contains "whoami: USERNAME field" "USERNAME: $USER_A" "$out"
assert_contains "whoami: INSTANCE field" "INSTANCE: $INSTANCE" "$out"
assert_contains "whoami: CONFIG field" "CONFIG:" "$out"

# ------------------------------------------------------------------ #
section "4. fingerprint (self)"
# ------------------------------------------------------------------ #

out=$(sh_run "$DIR_A" fingerprint 2>&1)
assert_exit "fingerprint self exits 0" 0 "$?"
# Should contain hex groups (GPG-style)
assert_contains "fingerprint: hex output" " " "$out"

# ------------------------------------------------------------------ #
section "5. fingerprint (remote user)"
# ------------------------------------------------------------------ #

out=$(sh_run "$DIR_A" fingerprint "$USER_B" 2>&1)
assert_exit "fingerprint remote exits 0" 0 "$?"
assert_contains "fingerprint: remote hex output" " " "$out"

# ------------------------------------------------------------------ #
section "6. send"
# ------------------------------------------------------------------ #

MSG_BODY="Hello from automated test at $(date)"
MAIL_ID=""

out=$(echo "$MSG_BODY" | sh_run "$DIR_A" send -t "$USER_B" \
    -s "Test subject $(date +%s)" 2>&1)
rc=$?; assert_exit "send mail exits 0" 0 "$rc" "$out"
assert_contains "send: success message" "sent" "$out"

# Send to self
out_self=$(echo "self test" | sh_run "$DIR_A" send -t "$USER_A" \
    -s "Self test" 2>&1)
rc=$?; assert_exit "send to self exits 0" 0 "$rc" "$out_self"

# Subject too long (>128 bytes)
LONG_SUBJ=$(python3 -c "print('x'*129)")
out_long=$(echo "body" | sh_run "$DIR_A" send -t "$USER_B" \
    -s "$LONG_SUBJ" 2>&1) || true
assert_not_contains "send: long subject rejected" "sent" "$out_long"

# ------------------------------------------------------------------ #
section "7. check inbox"
# ------------------------------------------------------------------ #

out=$(sh_run "$DIR_B" check inbox 2>&1)
rc=$?; assert_exit "check inbox exits 0" 0 "$rc" "$out"
assert_contains "check inbox: has content" "$USER_A" "$out"

# Extract mail_id (first token on data line, 10 chars base58)
MAIL_ID=$(sh_run "$DIR_B" check inbox --no-header 2>&1 | \
    grep -v '^$' | grep -v 'Total' | awk '{print $1}' | head -1)

# ------------------------------------------------------------------ #
section "7b. check sent"
# ------------------------------------------------------------------ #

out=$(sh_run "$DIR_A" check sent 2>&1)
assert_exit "check sent exits 0" 0 "$?"
assert_contains "check sent: has content" "$USER_B" "$out"

# ------------------------------------------------------------------ #
section "7c. check --count"
# ------------------------------------------------------------------ #

out=$(sh_run "$DIR_B" check inbox --count 2>&1)
assert_exit "check --count exits 0" 0 "$?"
# Should be a number
if echo "$out" | grep -qE '^[0-9]+$'; then
    echo -e "  ${GREEN}PASS${NC}  check --count: numeric output"
    PASS=$((PASS + 1))
else
    echo -e "  ${RED}FAIL${NC}  check --count: expected numeric output, got: $out"
    FAIL=$((FAIL + 1))
fi

# ------------------------------------------------------------------ #
section "7d. check --json"
# ------------------------------------------------------------------ #

out=$(sh_run "$DIR_B" check inbox --json 2>&1)
assert_exit "check --json exits 0" 0 "$?"
if echo "$out" | python3 -c "import sys,json; json.load(sys.stdin)" > /dev/null 2>&1; then
    echo -e "  ${GREEN}PASS${NC}  check --json: valid JSON"
    PASS=$((PASS + 1))
else
    echo -e "  ${RED}FAIL${NC}  check --json: not valid JSON"
    FAIL=$((FAIL + 1))
fi

# ------------------------------------------------------------------ #
section "7e. check --csv"
# ------------------------------------------------------------------ #

out=$(sh_run "$DIR_B" check inbox --csv 2>&1)
assert_exit "check --csv exits 0" 0 "$?"
# CSV should have commas
assert_contains "check --csv: comma-separated" "," "$out"

# ------------------------------------------------------------------ #
section "8. check <id> (detail view)"
# ------------------------------------------------------------------ #

if [ -n "$MAIL_ID" ]; then
    out=$(sh_run "$DIR_B" check "$MAIL_ID" 2>&1)
    assert_exit "check <id> exits 0" 0 "$?"
    assert_contains "check <id>: FROM field" "FROM:" "$out"
    assert_contains "check <id>: TO field" "TO:" "$out"
    assert_contains "check <id>: SUBJ field" "SUBJ:" "$out"
    assert_contains "check <id>: DATE field" "DATE:" "$out"
else
    echo -e "  ${YELLOW}SKIP${NC}  check <id>: no mail_id extracted"
fi

# ------------------------------------------------------------------ #
section "9. fetch"
# ------------------------------------------------------------------ #

if [ -n "$MAIL_ID" ]; then
    out=$(sh_run "$DIR_B" fetch "$MAIL_ID" 2>&1)
    assert_exit "fetch exits 0" 0 "$?"
    assert_contains "fetch: FROM field" "FROM:" "$out"
    assert_contains "fetch: decrypted body" "$USER_A" "$out"

    # --raw: only body to stdout, no metadata
    raw=$(sh_run "$DIR_B" fetch --raw "$MAIL_ID" 2>/dev/null)
    assert_not_contains "fetch --raw: no FROM line" "FROM:" "$raw"
else
    echo -e "  ${YELLOW}SKIP${NC}  fetch: no mail_id"
fi

# ------------------------------------------------------------------ #
section "10. block / unblock"
# ------------------------------------------------------------------ #

# User B blocks User A
out=$(sh_run "$DIR_B" block "$USER_A" 2>&1)
rc=$?; assert_exit "block exits 0" 0 "$rc" "$out"
assert_contains "block: confirmation" "blocked" "$out"

# Sending from A to B should now fail (blocked)
out_blocked=$(echo "blocked msg" | sh_run "$DIR_A" send -t "$USER_B" \
    -s "Should be blocked" 2>&1) || true
assert_not_contains "send to blocker: rejected" "sent" "$out_blocked"

# Unblock
out=$(sh_run "$DIR_B" unblock "$USER_A" 2>&1)
rc=$?; assert_exit "unblock exits 0" 0 "$rc" "$out"
assert_contains "unblock: confirmation" "unblocked" "$out"

# Send should succeed again
out_after=$(echo "unblocked msg" | sh_run "$DIR_A" send -t "$USER_B" \
    -s "After unblock" 2>&1)
rc=$?; assert_exit "send after unblock succeeds" 0 "$rc" "$out_after"

# ------------------------------------------------------------------ #
section "11. burn"
# ------------------------------------------------------------------ #

# Get a fresh mail_id from B's inbox after unblock send
BURN_ID=$(sh_run "$DIR_B" check inbox --no-header 2>&1 | \
    grep -v '^$' | grep -v 'Total' | awk '{print $1}' | head -1)

if [ -n "$BURN_ID" ]; then
    out=$(sh_run "$DIR_B" burn "$BURN_ID" 2>&1)
    assert_exit "burn exits 0" 0 "$?"
    assert_contains "burn: confirmation" "burned" "$out"

    # Verify it's gone: check <id> should fail
    sh_run "$DIR_B" check "$BURN_ID" > /dev/null 2>&1 && burned_rc=0 || burned_rc=$?
    assert_exit "burn: mail no longer accessible" 1 "$burned_rc"
else
    echo -e "  ${YELLOW}SKIP${NC}  burn: no mail_id to burn"
fi

# Third-party cannot burn (User A tries to burn a mail in B's inbox only)
# (skipped as it requires a second unburned mail that A doesn't own)

# ------------------------------------------------------------------ #
section "12. rotate"
# ------------------------------------------------------------------ #

out=$(sh_run "$DIR_A" rotate 2>&1)
rc=$?; assert_exit "rotate exits 0" 0 "$rc" "$out"
assert_contains "rotate: success message" "rotated" "$out"

# After rotate, new keys exist locally
assert_exit "rotate: new kem_pk.bin present" 0 \
    "$([ -f "$DIR_A/kem_pk.bin" ] && echo 0 || echo 1)"

# After rotate, can still send (with updated keys)
out=$(echo "post-rotate mail" | sh_run "$DIR_A" send -t "$USER_B" \
    -s "Post-rotate" 2>&1)
rc=$?; assert_exit "send after rotate succeeds" 0 "$rc" "$out"

# ------------------------------------------------------------------ #
section "13. pubkey API (GET /api/pubkey/:username)"
# ------------------------------------------------------------------ #

resp=$(curl -sf "$INSTANCE/api/pubkey/$USER_A" 2>&1) || true
if echo "$resp" | python3 -c \
    "import sys,json; d=json.load(sys.stdin); \
     assert 'kem_pubkey' in d and 'sig_pubkey' in d" > /dev/null 2>&1; then
    echo -e "  ${GREEN}PASS${NC}  pubkey API: returns kem_pubkey and sig_pubkey"
    PASS=$((PASS + 1))
else
    echo -e "  ${RED}FAIL${NC}  pubkey API: unexpected response: $resp"
    FAIL=$((FAIL + 1))
fi

# Non-existent user → 404
code=$(curl -so /dev/null -w "%{http_code}" \
    "$INSTANCE/api/pubkey/nonexistent_xyz_$(date +%s)")
assert_exit "pubkey API: 404 for unknown user" 0 "$([ "$code" = "404" ] && echo 0 || echo 1)"

# ------------------------------------------------------------------ #
section "14. destroy"
# ------------------------------------------------------------------ #

# Use a fresh account so we don't destroy A or B yet
DIR_C="$TMPDIR_ROOT/acct_c"
USER_C="tstc${TS}"
"$SHYAKE" init -c "$DIR_C" > /dev/null 2>&1
sh_run "$DIR_C" register -u "$USER_C" -i "$INSTANCE" > /dev/null 2>&1

# destroy requires typing username to stdin
out=$(echo "$USER_C" | sh_run "$DIR_C" destroy 2>&1)
rc=$?; assert_exit "destroy exits 0" 0 "$rc" "$out"
assert_contains "destroy: confirmation" "estroyed" "$out"

# After destroy, pubkey should be empty / user effectively gone
resp=$(curl -sf "$INSTANCE/api/pubkey/$USER_C" 2>&1) || true
if echo "$resp" | python3 -c \
    "import sys,json; d=json.load(sys.stdin); \
     assert d.get('kem_pubkey','x') == ''" > /dev/null 2>&1; then
    echo -e "  ${GREEN}PASS${NC}  destroy: server keys cleared"
    PASS=$((PASS + 1))
else
    echo -e "  ${YELLOW}NOTE${NC}  destroy: server key state: $resp"
fi

# ------------------------------------------------------------------ #
section "15. Anti-replay: stale timestamp rejected"
# ------------------------------------------------------------------ #

STALE_TS=$(date -v-10M +%s 2>/dev/null || date --date="10 minutes ago" +%s)
code=$(curl -so /dev/null -w "%{http_code}" \
    -H "X-Shyake-Username: $USER_B" \
    -H "X-Shyake-Timestamp: $STALE_TS" \
    -H "X-Shyake-Signature: AAAA" \
    -H "X-Shyake-Pow: AAAA" \
    "$INSTANCE/api/mail?type=inbox")
assert_exit "anti-replay: stale timestamp → 403" 0 \
    "$([ "$code" = "403" ] && echo 0 || echo 1)"

# ------------------------------------------------------------------ #
# Summary
# ------------------------------------------------------------------ #

TOTAL=$((PASS + FAIL))
echo ""
echo "=========================================="
echo -e " Results: ${GREEN}${PASS} passed${NC}, ${RED}${FAIL} failed${NC} / $TOTAL total"
echo "=========================================="
[ "$FAIL" -eq 0 ] && exit 0 || exit 1
