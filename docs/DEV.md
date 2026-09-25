## Shyake Developer Guide

This document helps you develop for Shyake.

**Table of contents**:

- [Client](#client)
  * [Dependencies](#dependencies)
  * [Build](#build)
  * [Install](#install)
  * [Testing](#testing)
- [Server](#server)
  * [Worker](#worker)
  * [Go server](#go-server)

## Client

### Dependencies

The build links `liboqs` statically on all platforms, so the binary
carries no runtime dependency on it. It links `libcurl` and
`libcrypto` dynamically on all platforms.

Dependencies (build-time only):

| Library | Purpose |
|---------|---------|
| `liboqs` | ML-KEM-768 & ML-DSA-65 |
| `libcurl` | HTTP transport |
| `openssl` (`libcrypto`) | SHA-256 fingerprinting |

On macOS with Homebrew:

```sh
brew install liboqs curl openssl@3
```
On Arch Linux:

```sh
sudo pacman -S cmake curl openssl
# build liboqs from source: see instructions below
```

On Debian/Ubuntu:

```sh
sudo apt install cmake libcurl4-openssl-dev libssl-dev
# build liboqs from source: see instructions below
```

On Termux (Android):

```sh
pkg install clang cmake make curl-dev openssl-dev
# build liboqs from source: see instructions below
```

**Building liboqs**

When you compile `liboqs` from source (for example, on GNU/Linux or
Termux), you must build a minimal version. A full build, with all
algorithms enabled, adds about 20MB to the binary.

To build `liboqs` with only the algorithms Shyake needs (ML-KEM-768
and ML-DSA-65), run:

```sh
git clone --depth 1 \
          --single-branch -b main \
          https://github.com/open-quantum-safe/liboqs.git
cd liboqs
mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Release \
      -DOQS_BUILD_ONLY_LIB=ON \
      -DOQS_USE_OPENSSL=ON \
      -DOQS_MINIMAL_BUILD="KEM_ml_kem_768;SIG_ml_dsa_65" \
      ..
make -j$(nproc)
sudo make install
```

When you compile in **Termux**, you must set the installation prefix
(`$PREFIX`) and omit `sudo`:

```sh
cmake -DCMAKE_BUILD_TYPE=Release \
      -DOQS_BUILD_ONLY_LIB=ON \
      -DOQS_USE_OPENSSL=ON \
      -DOQS_MINIMAL_BUILD="KEM_ml_kem_768;SIG_ml_dsa_65" \
      -DCMAKE_INSTALL_PREFIX=$PREFIX \
      ..
make -j4
make install
```

### Build

```sh
cd client
make
```

Outputs:

| File | Description |
|------|-------------|
| `bin/shyake` | CLI binary (`liboqs` statically linked on all platforms) |
| `lib/libshyake.a` | Static library for FFI |
| `lib/libshyake.so` or `lib/libshyake.dylib` | Shared library for FFI |

### Install

Copy the binary to any directory in `$PATH`:

```sh
cp bin/shyake /usr/local/bin/
```

### Testing

Run the end-to-end test suite against a local server. Either server
works, and a change to the protocol must pass against both:

```sh
# Terminal 1: the Worker
cd server/cf && npx wrangler dev --local
# or the Go server
cd server/go && SHYAKE_INSTANCE_DOMAIN=127.0.0.1:8787 go run ./cmd/shyake-server

# Terminal 2
cd client && make
bash tests/e2e_test.sh
```

`SHYAKE_TEST_INSTANCE` points the suite at another URL.

The federation test starts two Go servers itself and runs the client
between them. It covers relays in both directions, a relay to an
instance that is down (the client keeps a draft and sends it later),
and a block on relayed mail:

```sh
cd client && make && cd ..
bash tests/federation_test.sh
```

### Non-interactive passphrase

`SHYAKE_PASSPHRASE` skips the interactive prompt wherever a command
needs to unlock the secret key. `init` also uses it as the initial
passphrase. Use this variable for scripting tests, not for end
users. Inline values land in shell history. Exported values stay
visible to child processes.

```sh
export SHYAKE_PASSPHRASE=$(openssl rand -base64 12)
shyake init
shyake check inbox
```

## Server

### Worker

```sh
cd server/cf
./deploy.sh --local      # dependencies, wrangler.toml, local database
npx wrangler dev --local
```

The worker listens on `http://localhost:8787` by default.

`deploy.sh` generates `wrangler.toml` from `wrangler.template.toml`.
Git does not track `wrangler.toml`. To deploy to Cloudflare, run the
same script without `--local`. See [DEPLOY.md](DEPLOY.md).

### Go server

Requires Go 1.26 or newer. The server has no cgo dependency.

```sh
cd server/go
go test ./...                    # unit tests
go vet ./...
gofmt -l .                       # must print nothing
SHYAKE_INSTANCE_DOMAIN=127.0.0.1:8787 SHYAKE_DATABASE=/tmp/dev.db \
    go run ./cmd/shyake-server
```

Package layout, from the wire inward:

| Package | Purpose |
|---|---|
| `internal/protocol` | Addresses, PoW, signatures, signed messages. No I/O. |
| `internal/api` | HTTP handlers, authentication, rate limits |
| `internal/federation` | Outbound client, remote key cache, relays |
| `internal/store` | Storage interface and its backend test suite |
| `internal/store/sqlite` | The SQLite backend and its migrations |
| `internal/config` | `SHYAKE_*` environment settings |

**Signature test vectors.** `internal/protocol/testdata/liboqs_vectors.json`
holds signatures that liboqs made over messages that the client's
own cJSON built. The Go tests check that circl accepts them and that
the server rebuilds each signed message byte for byte. Regenerate
the file after a change to the signed messages:

```sh
cd server/go/internal/protocol/testdata
cc -std=c11 -o /tmp/gen gen_vectors.c \
   ../../../../../client/src/lib/vendor/cJSON/cJSON.c \
   -I../../../../../client/src/lib/vendor/cJSON \
   /usr/local/lib/liboqs.a -lcrypto
/tmp/gen > liboqs_vectors.json
```

**A new storage backend** implements `store.Store` and passes
`storetest.Run`, the same suite the SQLite backend runs. PostgreSQL
is planned this way. Keep SQL inside the backend package: the
interface speaks users, mail, and blocks.

**Federation on one machine.** Instances contact each other over
HTTPS, and the server refuses private addresses. For local tests,
`SHYAKE_FEDERATION_INSECURE=true` allows plain HTTP and loopback
addresses. Never set it on a public instance.

## Releasing

The repository has one version line: the release tags ([SPEC.md
§12.1](SPEC.md)). Each component records the release in which it
last changed:

- the client: `VERSION` in `client/Makefile`.
- both servers: `server/VERSION`.

To release:

1. Set the version of each changed component to the new tag. Leave
   an unchanged component at its old version.
2. Publish a GitHub release with that tag.

The release workflow builds the client when `client/Makefile` has the
tag, and the Go server when `server/VERSION` has it. If neither has
the tag, the workflow fails.

Worker instances follow the tags, not `main`: `deploy.sh --update`
checks out the newest `vX.Y.Z` tag. A merge to `main` reaches them
only when you tag it. A pre-release tag (`vX.Y.Z-rc.1`) never does.

A server change that affects clients must reach the servers first.
Bump the protocol level (`protocol.Level` in the Go server,
`PROTOCOL_LEVEL` in the Worker) when servers start to accept a new
request format. Clients read it from `GET /api/version`.
