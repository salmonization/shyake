#!/bin/sh
# Install or upgrade shyake-server from a GitHub release build.
#
#   curl -fsSL https://raw.githubusercontent.com/salmonization/shyake/main/server/go/deploy/install.sh | sh
#
# The script asks for the release channel (stable or preview), finds
# the newest release with a server build for this machine, checks its
# SHA-256 against the digest from GitHub, and installs it for systemd.
# A first install also asks for the instance domain. An upgrade keeps
# /etc/shyake/shyake.env and the unit file.
#
# Set SHYAKE_CHANNEL=stable or SHYAKE_CHANNEL=preview to skip the
# question about the channel.

set -eu

REPO=salmonization/shyake
ENV_FILE=/etc/shyake/shyake.env
UNIT=/etc/systemd/system/shyake-server.service

die() {
	echo "install.sh: $*" >&2
	exit 1
}

# questions go to the terminal: stdin is this script under curl | sh
ask() {
	printf '%s' "$1" >/dev/tty
	read -r answer </dev/tty || answer=
	printf '%s' "$answer"
}

# whether version $1 is older than $2; a release outranks its
# pre-releases (v0.4.0 > v0.4.0-indev.2)
older() {
	[ "$1" != "$2" ] || return 1
	if [ "${1%%-*}" = "${2%%-*}" ]; then
		[ "$1" != "${1%%-*}" ] || return 1
		[ "$2" != "${2%%-*}" ] || return 0
	fi
	[ "$(printf '%s\n%s\n' "$1" "$2" | sort -V | head -n 1)" = "$1" ]
}

[ "$(uname -s)" = Linux ] || die "release builds are for Linux only. Build from source."
(: </dev/tty) 2>/dev/null || die "this script asks questions: run it in a terminal."
for cmd in curl tar sha256sum awk; do
	command -v "$cmd" >/dev/null || die "$cmd is not installed."
done
SUDO=
[ "$(id -u)" -eq 0 ] || SUDO=sudo

# release channel
CHANNEL=${SHYAKE_CHANNEL:-}
if [ -z "$CHANNEL" ]; then
	echo "Release channel:"
	echo "  1) stable   releases only (recommended)"
	echo "  2) preview  pre-releases too, such as v0.4.0-indev.1"
	case "$(ask 'Choose [1]: ')" in
	2 | preview) CHANNEL=preview ;;
	'' | 1 | stable) CHANNEL=stable ;;
	*) die "unknown choice." ;;
	esac
fi
case "$CHANNEL" in
stable) TAG='v[0-9]+[.][0-9]+[.][0-9]+' ;;
preview) TAG='v[^/]+' ;;
*) die "SHYAKE_CHANNEL must be stable or preview." ;;
esac

# newest release with a build for this machine: URL and SHA-256
ARCH=$(uname -m)
case "$ARCH" in
x86_64) OLD=amd64 ;;
aarch64) OLD=arm64 ;;
*) die "no release build for $ARCH. Build from source." ;;
esac
RELEASES=$(curl -fsSL "https://api.github.com/repos/$REPO/releases?per_page=100") ||
	die "cannot reach the GitHub API."
# builds up to v0.4.0-indev.1 use Go names (amd64, arm64)
FOUND=$(printf '%s\n' "$RELEASES" | awk -v re="/download/$TAG/shyake-server-linux-($ARCH|$OLD)[.]tar[.]gz\"" '
	/"digest":/ { d = $2 }
	/"browser_download_url":/ { if ($2 ~ re) { print $2, d; exit } d = "" }' | tr -d '",')
[ -n "$FOUND" ] || die "no $CHANNEL release has a server build for $ARCH."
URL=${FOUND%% *}
SUM=${FOUND#* }
SUM=${SUM#sha256:}
ASSET=${URL##*/}
VERSION=$(printf '%s' "$URL" | sed 's|.*/download/\([^/]*\)/.*|\1|')
[ -n "$SUM" ] || die "GitHub reports no digest for $ASSET."

CURRENT=
command -v shyake-server >/dev/null && CURRENT=$(shyake-server -version 2>/dev/null | awk '{print $2}')
echo "Newest $CHANNEL server build: $VERSION"
[ -z "$CURRENT" ] || echo "Installed: $CURRENT"
if [ -n "$CURRENT" ] && ! older "$CURRENT" "$VERSION"; then
	# same version or a downgrade: only on an explicit yes
	[ "$CURRENT" = "$VERSION" ] && Q="Install $VERSION again? [y/N] " ||
		Q="$VERSION is older than $CURRENT. Install it? [y/N] "
	case "$(ask "$Q")" in
	y | Y | yes) ;;
	*) echo "Nothing installed."; exit 0 ;;
	esac
else
	case "$(ask "Install $VERSION? [Y/n] ")" in
	'' | y | Y | yes) ;;
	*) echo "Nothing installed."; exit 0 ;;
	esac
fi

# download and verify
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
cd "$WORK"
curl -fsSL -o "$ASSET" "$URL"
echo "$SUM  $ASSET" | sha256sum -c - >/dev/null || die "the SHA-256 of $ASSET does not match. Nothing installed."
echo "SHA-256 OK"
tar -xzf "$ASSET"
cd "${ASSET%.tar.gz}"

# install
id shyake >/dev/null 2>&1 ||
	$SUDO useradd --system --home-dir /var/lib/shyake --shell /usr/sbin/nologin shyake
$SUDO install -m 755 shyake-server /usr/local/bin/
if [ ! -f "$ENV_FILE" ]; then
	DOMAIN=
	while [ -z "$DOMAIN" ]; do
		DOMAIN=$(ask 'Instance domain (for example mail.example.com): ')
	done
	$SUDO install -D -m 640 -g shyake shyake.env.example "$ENV_FILE"
	$SUDO sed -i "s|^#* *SHYAKE_INSTANCE_DOMAIN=.*|SHYAKE_INSTANCE_DOMAIN=$DOMAIN|" "$ENV_FILE"
	echo "Wrote $ENV_FILE. Edit it for other settings."
fi
[ -f "$UNIT" ] || $SUDO install -m 644 shyake-server.service "$UNIT"

if [ ! -d /run/systemd/system ]; then
	echo "Installed $VERSION to /usr/local/bin. systemd is not running: start the server yourself."
	exit 0
fi
$SUDO systemctl daemon-reload
if systemctl is-active --quiet shyake-server; then
	$SUDO systemctl restart shyake-server
	echo "Restarted shyake-server $VERSION."
else
	$SUDO systemctl enable --now shyake-server
	echo "Started shyake-server $VERSION."
fi
