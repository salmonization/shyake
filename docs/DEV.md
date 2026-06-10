## Shyake Developer Guide

This document helps you develop for Shyake.

**Table of contents**:

- [Client](#client)
  * [Dependencies](#dependencies)
  * [Build](#build)
  * [Install](#install)
  * [Testing](#testing)
- [Server](#server)
  * [Local development](#local-development)

## Client

### Dependencies

`liboqs` is statically linked on all platforms so the binary carries
no runtime dependency on it. `libcurl` and `libcrypto` remain
dynamically linked on all platforms.

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
sudo pacman -S curl openssl
# build liboqs from source: https://github.com/open-quantum-safe/liboqs
```

On Debian/Ubuntu:

```sh
sudo apt install libcurl4-openssl-dev libssl-dev
# build liboqs from source: https://github.com/open-quantum-safe/liboqs
```

On Termux (Android):

```sh
pkg install clang make curl-dev openssl-dev
# build liboqs from source: https://github.com/open-quantum-safe/liboqs
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

Run the end-to-end test suite against a local dev server:

```sh
# Terminal 1
cd server && npx wrangler dev --local

# Terminal 2
cd client && make
bash tests/e2e_test.sh
```

## Server

### Local development

```sh
npx wrangler dev --local
```

The worker listens on `http://localhost:8787` by default.
