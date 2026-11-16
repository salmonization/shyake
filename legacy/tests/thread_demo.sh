#!/usr/bin/env bash
# Try replies and threads with two throwaway accounts, step by step.
#
#   bash tests/thread_demo.sh [instance_a] [instance_b]
#   curl -fsSL https://raw.githubusercontent.com/salmonization/shyake/dev/tests/thread_demo.sh |
#       bash -s -- [instance_a] [instance_b]
#
# Both accounts go on instance_a (default https://shyake.caffeine.ink)
# unless instance_b is given: then replies cross instances, and an
# instance below protocol 4 shows how threads degrade there.
# SHYAKE=/path/to/shyake picks the client binary. Without a terminal,
# it runs straight through and keeps the accounts.

set -u
# a terminal for pauses and questions; stdin is this script under curl | bash
TTY=0
(: </dev/tty) 2>/dev/null && TTY=1

main() {
S=${SHYAKE:-shyake}
IA=${1:-https://shyake.caffeine.ink}
IB=${2:-$IA}
IA=${IA%/}
IB=${IB%/}
W=$(mktemp -d "${TMPDIR:-/tmp}/shyake-demo.XXXXXX")
export SHYAKE_PASSPHRASE=""
N=$(date +%s)
N=${N: -6}
UA=ta$N
UB=tb$N

a() { "$S" -c "$W/a" "$@"; }
b() { "$S" -c "$W/b" "$@"; }
step() { printf '\n\033[1;33m== %s\033[0m\n' "$*"; }
show() {
	printf '\033[2m$ %s\033[0m\n' "$*"
	"$@"
}
pause() {
	[ "$TTY" = 1 ] || return 0
	printf '\033[2m[Enter] next, [q] quit: \033[0m'
	read -r k </dev/tty
	[ "$k" != q ] || finish
}
# mail ID by exact subject, from a box listing
id_of() {
	"$S" -c "$1" check "$2" --csv --no-header 2>/dev/null |
		awk -F, -v s="\"$3\"" '$3 == s { print $1; exit }'
}
need() {
	[ -n "$2" ] && return 0
	echo "Cannot find the ID of $1. Stopping." >&2
	finish
}
finish() {
	step "Cleanup"
	echo "Work directory: $W"
	printf 'Destroy both test accounts and delete the directory? [y/N] '
	k=n
	[ "$TTY" = 1 ] && read -r k </dev/tty
	if [ "$k" = y ]; then
		echo "$UA" | a destroy >/dev/null 2>&1 && echo "Destroyed $UA."
		echo "$UB" | b destroy >/dev/null 2>&1 && echo "Destroyed $UB."
		rm -rf "$W"
	else
		echo
		echo "Kept. Use them with:"
		echo "  $S -c $W/a ...   # $UA on $IA"
		echo "  $S -c $W/b ...   # $UB on $IB"
	fi
	exit 0
}

# addresses as the other account writes them
host() { local h=${1#*://}; echo "${h%%/*}"; }
if [ "$IA" = "$IB" ]; then
	TO_A=$UA
	TO_B=$UB
else
	TO_A=$UA@$(host "$IA")
	TO_B=$UB@$(host "$IB")
fi

step "Versions"
show "$S" version
curl -fsS "$IA/api/version"
echo
[ "$IB" = "$IA" ] || { curl -fsS "$IB/api/version"; echo; }

step "Accounts: $UA on $IA, $UB on $IB (registration needs PoW: wait)"
a init >/dev/null 2>&1 && b init >/dev/null 2>&1 || {
	echo "init failed" >&2
	exit 1
}
a register -u "$UA" -i "$IA" || exit 1
b register -u "$UB" -i "$IB" || exit 1

step "Build a thread: A <- B <- C <- E, and A <- D"
echo "Do you still have the Ethiopian beans?" |
	a send -t "$TO_B" -s "Beans" || finish
sleep 1
MA=$(id_of "$W/b" inbox "Beans")
need A "$MA"
echo "Yes, 250 g left." | b reply "$MA" -s "Re: Beans (B)" || finish
sleep 1
MB=$(id_of "$W/a" inbox "Re: Beans (B)")
need B "$MB"
echo "Great, I take them." | a reply "$MB" -s "Re: Beans (C)" || finish
sleep 1
MC=$(id_of "$W/a" sent "Re: Beans (C)")
need C "$MC"
MA_A=$(id_of "$W/a" sent "Beans")
need "A (sent copy)" "$MA_A"
echo "Also: any Kenyan beans?" | a reply "$MA_A" -s "Re: Beans (D)" || finish
sleep 1
MD=$(id_of "$W/a" sent "Re: Beans (D)")
need D "$MD"
MC_B=$(id_of "$W/b" inbox "Re: Beans (C)")
need "C (B's copy)" "$MC_B"
echo "Pick them up on Friday." | b reply "$MC_B" -s "Re: Beans (E)" || finish
sleep 1
ME=$(id_of "$W/a" inbox "Re: Beans (E)")
need E "$ME"
echo "A's IDs: A=$MA_A B=$MB C=$MC D=$MD E=$ME"
pause

step "check thread: same tree from any mail, @ moves"
show a check thread "$MC"
pause
show a check thread "$MA_A"
pause
show b check thread "$MA"
pause
show a check thread "$MC" --no-header --no-color
show a check thread "$MC" --json
pause

step "check / fetch of one reply: the RE: line"
show a check "$MC"
pause

step "fetch thread"
show a fetch thread "$MD"
pause
show a fetch thread "$MD" --flat
pause

step "save thread"
show a save thread "$ME"
show a save thread "$ME"
show a check saved
pause

step "Burn B: A has a saved copy, B has none"
show b burn "$(id_of "$W/b" sent "Re: Beans (B)")"
show a check thread "$ME"
pause
show b check thread "$(id_of "$W/b" sent "Re: Beans (E)")"
pause
show b check thread "$MA"
pause
show b fetch thread "$(id_of "$W/b" sent "Re: Beans (E)")"
pause
show b save thread "$(id_of "$W/b" sent "Re: Beans (E)")"
pause

step "read thread with the instance unreachable"
cp "$W/a/config" "$W/a/config.bak"
sed 's|^INSTANCE=.*|INSTANCE=http://127.0.0.1:1|' "$W/a/config.bak" >"$W/a/config"
show a read thread "$MA_A"
pause
show a read thread "$MA_A" --flat
show a check thread "$MA_A"
mv "$W/a/config.bak" "$W/a/config"
pause

step "Errors and usage"
show a check thread nosuchmail
show a read thread nosuchmail
show a check thread
show a check thread "$MC" --flat
show a fetch thread "$MC" --json
show a fetch thread "$MC" --raw
pause

step "Manual pages"
for c in check fetch save read rotate destroy; do
	show a man "$c"
	pause
done

step "rotate: the warning, then a wrong username (nothing changes)"
echo nobody | a rotate
pause

if [ "$TTY" = 1 ]; then
	step "Optional: reply in your editor (vim/nvim split, else scissors)"
	printf 'Reply to E as %s in the editor now? [y/N] ' "$UA"
	read -r k </dev/tty
	if [ "$k" = y ]; then
		a reply "$ME" </dev/tty
		show a check thread "$ME"
	fi
fi

finish
}

main "$@"
