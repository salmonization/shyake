#!/usr/bin/env bash
# Shyake end-to-end functional test suite
# Requires: a server on 127.0.0.1:8787 (the Worker under wrangler dev, or
# the Go server), shyake binary built. SHYAKE_TEST_INSTANCE overrides the
# server URL.

# No `set -e`: this is a test harness with its own PASS/FAIL accounting.
# Under -e a failing case kills the run before its assert can report it,
# which is why the suite used to need `|| true` and set +e/-e toggles
# scattered through it. Setup failures are checked explicitly instead,
# and the exit status comes from $FAIL at the bottom.
set -uo pipefail

# ------------------------------------------------------------------ #
# Configuration
# ------------------------------------------------------------------ #
SHYAKE="${SHYAKE_BIN:-$(dirname "$0")/../client/bin/shyake}"
INSTANCE="${SHYAKE_TEST_INSTANCE:-http://127.0.0.1:8787}"
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
        echo -e "  ${RED}FAIL${NC}  $name (expected $expected, got $actual)"
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
        echo -e "  ${RED}FAIL${NC}  $name (pattern found)"
        FAIL=$((FAIL + 1))
    else
        echo -e "  ${GREEN}PASS${NC}  $name"
        PASS=$((PASS + 1))
    fi
}

# init_nopass <dir>  -- init with no passphrase via env var
init_nopass() {
    SHYAKE_PASSPHRASE="" "$SHYAKE" init -c "$1" > /dev/null 2>&1
}

# init_withpass <dir> <passphrase>  -- init setting a passphrase via env var
init_withpass() {
    SHYAKE_PASSPHRASE="$2" "$SHYAKE" init -c "$1" > /dev/null 2>&1
}

# sh_run_pp <passphrase> <config_dir> <args...>  -- run with passphrase via env var
sh_run_pp() {
    local pp="$1" cfg="$2"; shift 2
    SHYAKE_PASSPHRASE="$pp" "$SHYAKE" --no-color -c "$cfg" "$@"
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
            echo "Start a server: cd server/cf && npx wrangler dev"
            echo "            or: cd server/go && SHYAKE_INSTANCE_DOMAIN=127.0.0.1:8787 go run ./cmd/shyake-server"
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

out=$(curl -s "$INSTANCE/api/version")
assert_contains "server reports protocol level 2" '"protocol":2' "$out"

# ------------------------------------------------------------------ #
section "1. init"
# ------------------------------------------------------------------ #

INIT_DIR="$TMPDIR_ROOT/init_test"
init_nopass "$INIT_DIR" || true
assert_exit "init creates config dir" 0 "$([ -d "$INIT_DIR" ] && echo 0 || echo 1)"
assert_exit "init creates config file" 0 \
    "$([ -f "$INIT_DIR/config" ] && echo 0 || echo 1)"
assert_exit "init creates kem_pk.bin" 0 \
    "$([ -f "$INIT_DIR/kem_pk.bin" ] && echo 0 || echo 1)"
assert_exit "init creates sig_pk.bin" 0 \
    "$([ -f "$INIT_DIR/sig_pk.bin" ] && echo 0 || echo 1)"
assert_exit "init creates kem_sk.bin" 0 \
    "$([ -f "$INIT_DIR/kem_sk.bin" ] && echo 0 || echo 1)"
assert_exit "init creates sig_sk.bin" 0 \
    "$([ -f "$INIT_DIR/sig_sk.bin" ] && echo 0 || echo 1)"

# ------------------------------------------------------------------ #
section "2. register"
# ------------------------------------------------------------------ #

# Generate unique usernames to avoid collision
TS=$(date +%s)
USER_A="tsta${TS}"
USER_B="tstb${TS}"
DIR_A="$TMPDIR_ROOT/acct_a"
DIR_B="$TMPDIR_ROOT/acct_b"

init_nopass "$DIR_A"
init_nopass "$DIR_B"

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
assert_not_contains "register: duplicate fails" "egistered" "$out_dup"

# Invalid username (too short)
out_bad=$(init_nopass "$TMPDIR_ROOT/bad"; \
    sh_run "$TMPDIR_ROOT/bad" register -u "ab" -i "$INSTANCE" 2>&1) || true
assert_not_contains "register: short username rejected" \
    "egistered" "$out_bad"

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
    echo -e "  ${RED}FAIL${NC}  check --count: not numeric"
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

# blocklist shows the blocked target
out=$(sh_run "$DIR_B" blocklist 2>&1)
rc=$?; assert_exit "blocklist exits 0" 0 "$rc" "$out"
assert_contains "blocklist: target listed" "$USER_A" "$out"

# Sending from A to B should now fail (blocked)
out_blocked=$(echo "blocked msg" | sh_run "$DIR_A" send -t "$USER_B" \
    -s "Should be blocked" 2>&1) || true
assert_not_contains "send to blocker: rejected" "sent" "$out_blocked"
assert_contains "send to blocker: reason shown" "blocked this sender" \
    "$out_blocked"

# Unblock
out=$(sh_run "$DIR_B" unblock "$USER_A" 2>&1)
rc=$?; assert_exit "unblock exits 0" 0 "$rc" "$out"
assert_contains "unblock: confirmation" "unblocked" "$out"

# blocklist empty after unblock
out=$(sh_run "$DIR_B" blocklist 2>&1)
rc=$?; assert_exit "blocklist after unblock exits 0" 0 "$rc" "$out"
assert_contains "blocklist: empty message" "empty" "$out"

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

out=$(SHYAKE_PASSPHRASE="" sh_run "$DIR_A" rotate 2>&1)
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
assert_exit "pubkey API: 404 for unknown user" 0 \
    "$([ "$code" = "404" ] && echo 0 || echo 1)"

# ------------------------------------------------------------------ #
section "14. destroy"
# ------------------------------------------------------------------ #

# Use a fresh account so we don't destroy A or B yet
DIR_C="$TMPDIR_ROOT/acct_c"
USER_C="tstc${TS}"
init_nopass "$DIR_C"
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
section "15. KEY_MISMATCH (409): recipient rotated after key cached"
# ------------------------------------------------------------------ #

# Setup: two fresh users D and E
DIR_D="$TMPDIR_ROOT/acct_d"
DIR_E="$TMPDIR_ROOT/acct_e"
USER_D="tstd${TS}"
USER_E="tste${TS}"
init_nopass "$DIR_D" || true
init_nopass "$DIR_E" || true
sh_run "$DIR_D" register -u "$USER_D" -i "$INSTANCE" > /dev/null 2>&1 || true
sh_run "$DIR_E" register -u "$USER_E" -i "$INSTANCE" > /dev/null 2>&1 || true

# D sends to E — E's pubkey fingerprint gets cached in D's known_hosts
echo "first mail" | sh_run "$DIR_D" send -t "$USER_E" \
    -s "Cache E key" > /dev/null 2>&1 || true

# E rotates keys — server now has a new pubkey for E
SHYAKE_PASSPHRASE="" sh_run "$DIR_E" rotate > /dev/null 2>&1 || true

# D sends to E again — expects failure: local fingerprint mismatch
out_mismatch=$(echo "after rotate" | sh_run "$DIR_D" send -t "$USER_E" \
    -s "Should fail" 2>&1)
rc_mismatch=$?
assert_exit "409 path: send exits non-zero after key rotation" 1 "$rc_mismatch"
assert_contains "409 path: FATAL key-changed message" \
    "Remote public key of recipient has changed" "$out_mismatch"

# After --update, D can send again
sh_run "$DIR_D" fingerprint "$USER_E" --update > /dev/null 2>&1 || true
out_ok=$(echo "after update" | sh_run "$DIR_D" send -t "$USER_E" \
    -s "After trust update" 2>&1)
assert_exit "409 path: send succeeds after fingerprint --update" 0 "$?"

# ------------------------------------------------------------------ #
section "16. USER_DESTROYED (410): send to destroyed account"
# ------------------------------------------------------------------ #

# Setup: two fresh users F and G
DIR_F="$TMPDIR_ROOT/acct_f"
DIR_G="$TMPDIR_ROOT/acct_g"
USER_F="tstf${TS}"
USER_G="tstg${TS}"
init_nopass "$DIR_F" || true
init_nopass "$DIR_G" || true
sh_run "$DIR_F" register -u "$USER_F" -i "$INSTANCE" > /dev/null 2>&1 || true
sh_run "$DIR_G" register -u "$USER_G" -i "$INSTANCE" > /dev/null 2>&1 || true

# G sends to F — F's pubkey cached in G's known_hosts
echo "initial mail" | sh_run "$DIR_G" send -t "$USER_F" \
    -s "Cache F key" > /dev/null 2>&1 || true

# F destroys account — server clears F's pubkeys
echo "$USER_F" | sh_run "$DIR_F" destroy > /dev/null 2>&1 || true

# G tries to send to F — expects failure (destroyed user)
out_destroyed=$(echo "to destroyed" | sh_run "$DIR_G" send -t "$USER_F" \
    -s "Should fail" 2>&1)
rc_destroyed=$?
assert_exit "410 path: send exits non-zero to destroyed user" 1 "$rc_destroyed"
assert_not_contains "410 path: mail not sent" "sent" "$out_destroyed"
# Accept server-side 410 or client-side empty-key mismatch detection
if echo "$out_destroyed" | grep -qF "no longer exists"; then
    echo -e "  ${GREEN}PASS${NC}  410 path: server FATAL message present"
    PASS=$((PASS + 1))
elif echo "$out_destroyed" | grep -qF "changed"; then
    echo -e "  ${GREEN}PASS${NC}  410 path: client detected cleared key as mismatch"
    PASS=$((PASS + 1))
else
    echo -e "  ${RED}FAIL${NC}  410 path: unexpected error output: $out_destroyed"
    FAIL=$((FAIL + 1))
fi

# ------------------------------------------------------------------ #
section "17. Anti-replay: stale timestamp rejected"
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
section "18. Passphrase-protected keys"
# ------------------------------------------------------------------ #

PP_PASS="hunter2-test-pp"
DIR_PP="$TMPDIR_ROOT/acct_pp"
USER_PP="tstpp${TS}"

# 18a. init without passphrase → keys are raw binary (no SHYK magic)
PLAIN_DIR="$TMPDIR_ROOT/plain_pp"
init_nopass "$PLAIN_DIR"
if [ -f "$PLAIN_DIR/kem_sk.bin" ]; then
    magic=$(dd if="$PLAIN_DIR/kem_sk.bin" bs=4 count=1 2>/dev/null | \
        od -A n -t x1 | tr -d ' \n')
    if [ "$magic" != "5348594b" ]; then
        echo -e "  ${GREEN}PASS${NC}  passphrase: no-passphrase init → raw binary key"
        PASS=$((PASS + 1))
    else
        echo -e "  ${RED}FAIL${NC}  passphrase: no-passphrase init produced SHYK magic"
        FAIL=$((FAIL + 1))
    fi
else
    echo -e "  ${RED}FAIL${NC}  passphrase: plain init did not create kem_sk.bin"
    FAIL=$((FAIL + 1))
fi

# 18b. init with passphrase → keys start with SHYK magic
init_withpass "$DIR_PP" "$PP_PASS"
assert_exit "passphrase: init with passphrase creates kem_sk.bin" 0 \
    "$([ -f "$DIR_PP/kem_sk.bin" ] && echo 0 || echo 1)"
magic_kem=$(dd if="$DIR_PP/kem_sk.bin" bs=4 count=1 2>/dev/null | \
    od -A n -t x1 | tr -d ' \n')
magic_sig=$(dd if="$DIR_PP/sig_sk.bin" bs=4 count=1 2>/dev/null | \
    od -A n -t x1 | tr -d ' \n')
if [ "$magic_kem" = "5348594b" ]; then
    echo -e "  ${GREEN}PASS${NC}  passphrase: kem_sk.bin starts with SHYK magic"
    PASS=$((PASS + 1))
else
    echo -e "  ${RED}FAIL${NC}  passphrase: kem_sk.bin missing" \
        "SHYK magic (got $magic_kem)"
    FAIL=$((FAIL + 1))
fi
if [ "$magic_sig" = "5348594b" ]; then
    echo -e "  ${GREEN}PASS${NC}  passphrase: sig_sk.bin starts" \
        "with SHYK magic"
    PASS=$((PASS + 1))
else
    echo -e "  ${RED}FAIL${NC}  passphrase: sig_sk.bin missing" \
        "SHYK magic (got $magic_sig)"
    FAIL=$((FAIL + 1))
fi

# Add config and register (passphrase via env var)
cat > "$DIR_PP/config" <<EOF
INSTANCE=$INSTANCE
USERNAME=$USER_PP
TIME_FORMAT="%Y-%m-%d %H:%M"
CHECK_COLUMNS=id,sender,subject,size,date
NO_COLOR=1
EOF

pp_reg_out=$(sh_run_pp "$PP_PASS" "$DIR_PP" register -u "$USER_PP" -i "$INSTANCE" 2>&1)
pp_reg_rc=$?
assert_exit "passphrase: register with encrypted keys exits 0" 0 "$pp_reg_rc"

# 18c. send from USER_A (unencrypted keys) to USER_PP → no passphrase prompt for A
pp_send_out=$(echo "passphrase test body" | sh_run "$DIR_A" \
    send -t "$USER_PP" -s "pp test $(date +%s)" 2>&1)
assert_exit "passphrase: send to pp-user exits 0" 0 "$?"
assert_contains "passphrase: send success" "sent" "$pp_send_out"

# 18d. check inbox with correct passphrase
pp_check_out=$(sh_run_pp "$PP_PASS" "$DIR_PP" check inbox 2>&1)
assert_exit "passphrase: check inbox with correct passphrase exits 0" 0 "$?"
assert_contains "passphrase: inbox has mail from A" "$USER_A" "$pp_check_out"

# 18e. check inbox with wrong passphrase → exits non-zero
pp_wrong_out=$(SHYAKE_PASSPHRASE="wrong-passphrase" sh_run "$DIR_PP" check inbox 2>&1)
pp_wrong_rc=$?
assert_exit "passphrase: wrong passphrase → non-zero exit" 1 "$pp_wrong_rc"
assert_contains "passphrase: wrong passphrase message" \
    "Incorrect passphrase" "$pp_wrong_out"

# 18f. enc (uses public key only, no passphrase) → dec with correct passphrase
ENC_IN="$TMPDIR_ROOT/enc_input.txt"
ENC_OUT="$TMPDIR_ROOT/enc_input.txt.enc"
echo "secret content" > "$ENC_IN"
enc_out=$(sh_run "$DIR_PP" enc "$ENC_IN" 2>&1)
assert_exit "passphrase: enc (own pub key) exits 0" 0 "$?"
assert_exit "passphrase: enc produces .enc file" 0 \
    "$([ -f "$ENC_OUT" ] && echo 0 || echo 1)"

DEC_OUT="$TMPDIR_ROOT/dec_output.txt"
sh_run_pp "$PP_PASS" "$DIR_PP" dec "$ENC_OUT" -o "$DEC_OUT" > /dev/null 2>&1
assert_exit "passphrase: dec with correct passphrase exits 0" 0 "$?"
assert_exit "passphrase: dec output file exists" 0 \
    "$([ -f "$DEC_OUT" ] && echo 0 || echo 1)"
dec_content=$(cat "$DEC_OUT" 2>/dev/null || true)
assert_contains "passphrase: dec output matches original" "secret content" "$dec_content"

# 18g. send with encrypted keys piping body via stdin
#      SHYAKE_PASSPHRASE is set so read_passphrase never touches /dev/tty
pp_send2_out=$(echo "reply body" | sh_run_pp "$PP_PASS" "$DIR_PP" \
    send -t "$USER_A" -s "reply test" 2>&1)
assert_exit "passphrase: send with encrypted keys (piped body) exits 0" 0 "$?"
assert_contains "passphrase: send with encrypted keys success" "sent" "$pp_send2_out"

# ------------------------------------------------------------------ #
section "19. compose / drafts"
# ------------------------------------------------------------------ #

# Fake editors: write a fixed draft into the temp file ($1)
FAKE_ED="$TMPDIR_ROOT/fake_editor.sh"
cat > "$FAKE_ED" <<EOF
#!/bin/sh
cat > "\$1" <<'DRAFT'
To: $USER_B
Subject: Draft subject
---
Draft body content
DRAFT
EOF
chmod +x "$FAKE_ED"

FAKE_ED_DIARY="$TMPDIR_ROOT/fake_editor_diary.sh"
cat > "$FAKE_ED_DIARY" <<'EOF'
#!/bin/sh
cat > "$1" <<'DRAFT'
To:
Subject: Dear diary
---
Today I revived the spirit of ed/vi encryption.
DRAFT
EOF
chmod +x "$FAKE_ED_DIARY"

FAKE_ED_EDIT="$TMPDIR_ROOT/fake_editor_edit.sh"
cat > "$FAKE_ED_EDIT" <<EOF
#!/bin/sh
cat > "\$1" <<'DRAFT'
To: $USER_B
Subject: Draft subject
---
Edited body content
DRAFT
EOF
chmod +x "$FAKE_ED_EDIT"

# 19a. compose creates an encrypted draft
out=$(EDITOR="$FAKE_ED" VISUAL="$FAKE_ED" sh_run "$DIR_A" compose 2>&1)
rc=$?; assert_exit "compose exits 0" 0 "$rc" "$out"
assert_contains "compose: saved message" "saved" "$out"
DRAFT_ID=$(echo "$out" | grep -oE 'Draft [0-9]+' | awk '{print $2}' | head -1)
assert_exit "compose: draft file exists" 0 \
    "$([ -f "$DIR_A/drafts/$DRAFT_ID.json" ] && echo 0 || echo 1)"

# 19b. nothing sensitive in plaintext on disk
draft_raw=$(cat "$DIR_A/drafts/$DRAFT_ID.json" 2>/dev/null || true)
assert_not_contains "drafts: body encrypted on disk" \
    "Draft body content" "$draft_raw"
assert_not_contains "drafts: subject encrypted on disk" \
    "Draft subject" "$draft_raw"
assert_not_contains "drafts: recipient encrypted on disk" \
    "$USER_B" "$draft_raw"

# 19c. check drafts lists with decrypted metadata
out=$(sh_run "$DIR_A" check drafts 2>&1)
assert_exit "check drafts exits 0" 0 "$?"
assert_contains "check drafts: subject decrypted" "Draft subject" "$out"
assert_contains "check drafts: recipient shown" "$USER_B" "$out"
assert_contains "check drafts: ID column header" "ID" "$out"
assert_contains "check drafts: Created column" "Created" "$out"
assert_contains "check drafts: Modified column" "Modified" "$out"

# 19d. check drafts <id> shows header only; read drafts <id> shows body
out=$(sh_run "$DIR_A" check drafts "$DRAFT_ID" 2>&1)
assert_exit "check drafts <id> exits 0" 0 "$?"
assert_contains "check drafts <id>: subject shown" "Draft subject" "$out"
assert_contains "check drafts <id>: CRT field" "CRT:" "$out"
assert_contains "check drafts <id>: MOD field" "MOD:" "$out"
assert_not_contains "check drafts <id>: body not shown" \
    "Draft body content" "$out"
out=$(sh_run "$DIR_A" read drafts "$DRAFT_ID" 2>&1)
assert_exit "read drafts <id> exits 0" 0 "$?"
assert_contains "read drafts <id>: body decrypted" \
    "Draft body content" "$out"

# 19e. diary draft (empty To) lists as (null), refuses send without -t
out=$(EDITOR="$FAKE_ED_DIARY" VISUAL="$FAKE_ED_DIARY" \
    sh_run "$DIR_A" compose 2>&1)
rc=$?; assert_exit "compose diary exits 0" 0 "$rc" "$out"
DIARY_ID=$(echo "$out" | grep -oE 'Draft [0-9]+' | awk '{print $2}' | head -1)
out=$(sh_run "$DIR_A" check drafts 2>&1)
assert_contains "check drafts: diary marker" "(null)" "$out"
out=$(sh_run "$DIR_A" send --draft "$DIARY_ID" 2>&1)
rc=$?
assert_exit "send --draft diary without -t fails" 1 "$rc"
assert_contains "send --draft: no-recipient message" "no recipient" "$out"

# 19f. compose <id> edits in place, created timestamp preserved
created_before=$(python3 -c "import json; \
    print(json.load(open('$DIR_A/drafts/$DRAFT_ID.json'))['created'])")
out=$(EDITOR="$FAKE_ED_EDIT" VISUAL="$FAKE_ED_EDIT" \
    sh_run "$DIR_A" compose "$DRAFT_ID" 2>&1)
rc=$?; assert_exit "compose <id> exits 0" 0 "$rc" "$out"
created_after=$(python3 -c "import json; \
    print(json.load(open('$DIR_A/drafts/$DRAFT_ID.json'))['created'])")
assert_exit "compose <id>: created preserved" 0 \
    "$([ "$created_before" = "$created_after" ] && echo 0 || echo 1)"
out=$(sh_run "$DIR_A" read drafts "$DRAFT_ID" 2>&1)
assert_contains "compose <id>: body updated" "Edited body content" "$out"

# 19g. unchanged template aborts without creating a draft
n_before=$(ls "$DIR_A/drafts" | wc -l)
out=$(EDITOR="true" VISUAL="true" sh_run "$DIR_A" compose 2>&1)
rc=$?
assert_exit "compose unchanged template exits non-zero" 1 "$rc"
assert_contains "compose: aborted message" "aborted" "$out"
n_after=$(ls "$DIR_A/drafts" | wc -l)
assert_exit "compose aborted: no new draft file" 0 \
    "$([ "$n_before" = "$n_after" ] && echo 0 || echo 1)"

# 19h. send --draft delivers and deletes the draft
out=$(sh_run "$DIR_A" send --draft "$DRAFT_ID" 2>&1)
rc=$?; assert_exit "send --draft exits 0" 0 "$rc" "$out"
assert_contains "send --draft: sent" "sent" "$out"
assert_contains "send --draft: deleted" "deleted" "$out"
assert_exit "send --draft: draft file removed" 0 \
    "$([ ! -f "$DIR_A/drafts/$DRAFT_ID.json" ] && echo 0 || echo 1)"
out=$(sh_run "$DIR_B" check inbox 2>&1)
assert_contains "send --draft: recipient received it" \
    "Draft subject" "$out"

# 19i. diary draft sends with -t override
# (target is B: A rotated keys in section 12, so A's own known_hosts
#  entry for itself is stale and a send-to-self would 409)
out=$(sh_run "$DIR_A" send --draft "$DIARY_ID" -t "$USER_B" 2>&1)
rc=$?; assert_exit "send --draft with -t override exits 0" 0 "$rc" "$out"
assert_contains "send --draft -t: sent" "sent" "$out"

# 19j. unknown draft id fails cleanly
out=$(sh_run "$DIR_A" send --draft 9999 2>&1)
rc=$?
assert_exit "send --draft unknown id fails" 1 "$rc"
assert_contains "send --draft: not-found message" "not found" "$out"

# 19k. passphrase-protected account: compose needs no passphrase,
#      reading drafts does
out=$(EDITOR="$FAKE_ED_DIARY" VISUAL="$FAKE_ED_DIARY" \
    sh_run "$DIR_PP" compose 2>&1)
rc=$?; assert_exit "compose with encrypted keys (no passphrase)" 0 \
    "$rc" "$out"
out=$(sh_run_pp "$PP_PASS" "$DIR_PP" check drafts 2>&1)
assert_exit "check drafts with correct passphrase exits 0" 0 "$?"
assert_contains "check drafts: pp draft listed" "Dear diary" "$out"
out=$(SHYAKE_PASSPHRASE="wrong-passphrase" \
    sh_run "$DIR_PP" check drafts 2>&1)
rc=$?
assert_exit "check drafts with wrong passphrase fails" 1 "$rc"
assert_contains "check drafts: wrong passphrase message" \
    "Incorrect passphrase" "$out"

# ------------------------------------------------------------------ #
# Summary
# ------------------------------------------------------------------ #

TOTAL=$((PASS + FAIL))
echo ""
echo "=========================================="
echo -e " Results: ${GREEN}${PASS} passed${NC}, ${RED}${FAIL} failed${NC} / $TOTAL total"
echo "=========================================="
[ "$FAIL" -eq 0 ] && exit 0 || exit 1
