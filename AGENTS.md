# AGENT.md

Guidance for AI coding agents working in this repository.

## Project

**Shyake** is an end-to-end encrypted mail system powered by
post-quantum cryptography, designed as a decentralized communication
method to resist censorship and surveillance. It consists of:

- A **C client** (`client/`): a POSIX-style CLI (`shyake`) plus a
  reusable library (`libshyake`) exposing a public FFI API.
- A **server** in two implementations with the same HTTP API: a
  Cloudflare Worker (`server/cf/`) and a Go server for self-hosting
  (`server/go/`). Both store only ciphertext and public keys; neither
  sees plaintext.

Key crypto: **ML-KEM-768** for key encapsulation, **ML-DSA-65** for
signatures, **ChaCha20-Poly1305** for symmetric encryption. Read
[docs/SPEC.md](docs/SPEC.md) before touching any crypto, wire format,
or authentication code.

## Tech Stack

### Client (`client/`)

- C11, POSIX.1-2008 (`_POSIX_C_SOURCE=200809L`)
- GNU Make; builds on macOS, GNU/Linux, and Termux (Android)
- Dependencies:
  - `liboqs`: ML-KEM-768 & ML-DSA-65 (**always statically linked**)
  - `libcurl`: HTTP transport (dynamic)
  - `libcrypto` (OpenSSL): SHA-256, ChaCha20-Poly1305, scrypt (dynamic)
  - `cJSON`: vendored at `client/src/lib/vendor/cJSON/`

### Server (`server/cf/`)

- TypeScript on Cloudflare Workers
- [Hono](https://hono.dev/) for routing
- Cloudflare D1 (SQLite) for storage, KV for version cache
- `mldsa65-wasm`: ML-DSA-65 signature verification in WebAssembly,
  loaded via the Wrangler `CompiledWasm` rule

### Go server (`server/go/`)

- Go 1.26+, no cgo: one static binary, `shyake-server`
- Standard library `net/http`; `log/slog` for logs
- SQLite via `modernc.org/sqlite` (pure Go). PostgreSQL is planned
  behind `internal/store`; a backend must pass `store/storetest`
- `github.com/cloudflare/circl` for ML-DSA-65 verification
- Configuration: `SHYAKE_*` environment variables (SPEC.md §11.2)

## Common Commands

### Client

```sh
cd client
make            # build bin/shyake, lib/libshyake.a, lib/libshyake.{so,dylib}
make test       # build and run tests/test_crypto.c
make clean      # remove obj/, bin/, lib/
```

The client's release version is `VERSION` in `client/Makefile`; the
servers' is `server/VERSION`. Set a component's version to the new
tag only when that component changed: the release workflow builds a
component only when its version equals the tag (docs/DEV.md,
Releasing).

### Server

```sh
cd server/cf
./deploy.sh --local         # set up for local dev (config + database)
npx wrangler dev --local    # local dev server on http://localhost:8787
./deploy.sh                 # deploy to Cloudflare; --update to upgrade
```

`wrangler.toml` is generated from `wrangler.template.toml` by
`deploy.sh` and is git-ignored; edit the generated file, not the
template.

### Go server

```sh
cd server/go
go test ./...               # unit tests (store, protocol, api, federation)
go vet ./... && gofmt -l .  # gofmt must print nothing
SHYAKE_INSTANCE_DOMAIN=127.0.0.1:8787 go run ./cmd/shyake-server
```

### Formatting

```sh
# C: .clang-format at repo root (requires clang-format installed)
clang-format -i client/src/**/*.c client/src/**/*.h

# TypeScript: .prettierrc at repo root
cd server/cf
npm run format          # rewrite src/
npm run format:check    # check only
```

Vendored code (`client/src/lib/vendor/`) is excluded from formatting.

### End-to-end tests

```sh
# Terminal 1
cd server/cf && npx wrangler dev --local

# Terminal 2
cd client && make
bash tests/e2e_test.sh      # from repo root: tests/e2e_test.sh
```

The e2e suite runs against either server; a protocol change must
pass against both. `tests/federation_test.sh` starts two Go servers
itself and tests relays between them.

## Project Structure

```
shyake/
├── client/                 # C client
│   ├── src/lib/            # core library (account, crypto, mail,
│   │   │                   #   network, passphrase, known_hosts,
│   │   │                   #   enc_dec)
│   │   └── vendor/cJSON/   # vendored JSON parser
│   ├── src/cli/            # CLI entry, init, display, prompts,
│   │                       #   drafts, self-update, man pages
│   ├── include/shyake.h    # public FFI API (opaque context pointer)
│   ├── tests/              # unit tests + test account fixtures
│   └── Makefile
├── server/
│   ├── cf/                 # Cloudflare Worker
│   │   ├── src/index.ts    # Hono routes
│   │   ├── src/utils.ts    # helpers (PoW, usernames, addresses)
│   │   ├── migrations/     # D1 schema migrations
│   │   ├── deploy.sh       # deploy / upgrade via the Wrangler CLI
│   │   └── wrangler.template.toml  # rendered to wrangler.toml
│   └── go/                 # Go server (self-hosting)
│       ├── cmd/shyake-server/      # entry point
│       ├── internal/protocol/      # addresses, PoW, signatures (no I/O)
│       ├── internal/api/           # HTTP handlers, auth, rate limits
│       ├── internal/federation/    # outbound client, key cache, relays
│       ├── internal/store/         # storage interface + sqlite backend
│       └── deploy/         # systemd unit, env example
├── tests/
│   ├── e2e_test.sh         # end-to-end test suite (bash)
│   └── federation_test.sh  # two Go instances, relays between them
├── webui/                  # experimental local web GUI (Bun + bun:ffi)
│   ├── src/ffi.ts          #   bun:ffi bindings for include/shyake.h
│   ├── src/server.ts       #   token-guarded HTTP API on 127.0.0.1
│   └── public/             #   vanilla HTML/JS UI (no build step)
└── docs/
    ├── SPEC.md             # technical specification (protocol, crypto)
    ├── DEV.md              # developer guide (deps, build, testing)
    ├── DEPLOY.md           # self-hosting deployment guide
    └── i18n/               # zh-CN & ja translations (READMEs + docs)
```

Architecture notes:

- **`src/lib/` is `libshyake`, not "the client's backend".** It must
  contain only core, universally applicable protocol logic: crypto
  (keygen, encrypt/decrypt, sign/verify), wire-format encoding, and
  the network send/receive operations defined in `docs/SPEC.md`. Any
  developer should be able to build their own fully protocol-compatible
  client (TUI, GUI, or any language via FFI) on top of `libshyake`
  alone.
- **`src/cli/` is one reference client, not the only one.** Anything
  specific to this particular CLI distribution belongs there:
  argument parsing, display, interactive prompts, self-update /
  install logic, and user-facing messages.
- The library never prints user-facing output or invokes shell
  commands; it returns `shyake_err` codes and data. Failure detail
  goes through `set_error()` and is read by the client via
  `shyake_last_error(ctx)`. The client decides how to present it.
- `set_error()` records only the reason, as full sentences ("You are
  blocked by bob."), never the action or an HTTP status. The CLI
  prints every failure as `<Action> failed. <reason>`, with no
  `Error:` prefix; only a changed recipient key prints as
  `FATAL: <reason>`.
- The CLI links against `libshyake.a` and talks to the library only
  through `include/shyake.h`.
- Internal library headers are `lib_internal.h` / `internal.h`; do not
  expose internals through `shyake.h` unless the FFI needs them.
- Server API changes must stay in sync with the client's network layer
  (`client/src/lib/network.c`), with `docs/SPEC.md`, and between the
  two servers: the Worker and the Go server must answer the same
  request the same way.
- Header-signed requests with a body sign its SHA-256 too (protocol
  level 2, SPEC.md §3.3). A change that makes servers accept a new
  request format bumps the protocol level in both servers.
- Servers relay mail synchronously and never queue it; the client
  keeps a draft when a send fails.
- In the Go server, `internal/protocol` holds the wire rules and does
  no I/O. The signed-message rebuild there must stay byte-exact with
  the client's cJSON; `testdata/liboqs_vectors.json` checks it.
- `webui/` is a third-party-style client: it must talk to the library
  only through `include/shyake.h` (via `bun:ffi`), never reaching into
  library internals. The struct layouts mirrored in `webui/src/ffi.ts`
  must be updated whenever `shyake.h` structs change.

## Coding Conventions

### C

Linux kernel coding style
([docs.kernel.org/process/coding-style.html](https://docs.kernel.org/process/coding-style.html)).
[.clang-format](.clang-format) is the upstream kernel `.clang-format`
verbatim (minus its `ForEachMacros` list, not applicable here) - run
it, don't hand-tune around it; see the file header for provenance.

- Tabs for indentation, 8 columns wide; 80-column limit where
  sensible; no trailing whitespace.
- K&R braces: opening brace on the same line for `if`/`for`/`while`/
  `switch`; **function opening braces go on the next line**. Omit
  braces for single-statement bodies.
- Comments: brief English phrases (3-7 words) at the start of major
  blocks only. No line-by-line commentary. Multi-line `/* ... */`
  blocks are used for file-format and protocol layouts.
- Security-sensitive code: zeroize secret material after use (see
  `zero_memory()` in `client/src/lib/passphrase.c`), never log secrets
  or plaintext, and keep all private-key operations client-side.

#### Type discipline

The kernel style already implies short fixed-width type names; this
project spells them the Rust way:

```c
typedef uint8_t   u8;   typedef int8_t    i8;
typedef uint16_t  u16;  typedef int16_t   i16;
typedef uint32_t  u32;  typedef int32_t   i32;
typedef uint64_t  u64;  typedef int64_t   i64;
typedef float     f32;  typedef double    f64;
typedef size_t    usize; typedef ptrdiff_t isize;
typedef uintptr_t uptr;
```

- These live in `client/src/lib/internal.h` and are for **internal
  code only**. `include/shyake.h` is a public FFI header: it keeps the
  `<stdint.h>` spellings, because `u8`/`usize` in a public header
  would collide with whatever the consumer already defines.
- New code uses the aliases; don't reformat untouched files just to
  convert them.

#### Banned and preferred library calls

- Banned: `strcpy`, `strcat`, `strncpy`, `sprintf`, `strtok`. Their
  interfaces cannot express a destination bound (or, for `strncpy`,
  do not NUL-terminate). Use `snprintf` for formatting, `memcpy` with
  an explicit length for copying, and `strchr`-based scanning instead
  of `strtok`.
- `memcpy`/`memmove`/`memset` are fine - length is explicit and the
  semantics have nothing to do with NUL termination.
- Pass binary data as `(const u8 *buf, usize len)`, never as a bare
  `char *` whose length the callee has to discover.

#### Parse, don't validate

- Cross a trust boundary exactly once, at the edge, and let the
  resulting type carry the proof: `shyake_ctx` is opaque and can only
  be produced by `shyake_init_ctx()`; new subsystems follow the same
  shape (opaque struct in the header, constructor that validates).
- Do not re-check the same input deeper in the call chain - if a
  function needs a guarantee, take a type that only the parser can
  hand out.

#### Errors

- Library functions return `shyake_err` (or `NULL`) and record detail
  via `set_error()`; the caller reads it with `shyake_last_error()`.
  This is the project's stand-in for `Result<T, E>` - do not invent a
  second error channel (no `errno` smuggling, no printing from the
  library).

#### Explicitly not adopted

- **Arena allocators**: secret material must be zeroized at a
  well-defined point, which a bulk arena free does not give us.
- **C23 tuple macros / tag compatibility**: the client is `-std=c11
  -pedantic` and has to build on Termux and older toolchains.
- **A length-carrying `String` struct**: the public API and the FFI
  consumers are `char *`-based; converting would break every binding
  for readability alone.

### Go

- `gofmt` formatting, `go vet` clean.
- Same commenting style as C: short purpose annotations at block
  level. Doc comments on exported identifiers follow Go convention.
- Errors are values: return them, wrap with `%w`, compare with
  `errors.Is`. Library packages never log; `internal/api` and
  `internal/federation` log through the `*slog.Logger` they receive.
- Never log request paths, usernames, client addresses, or bodies.
  Log the route pattern (`r.Pattern`) instead.

### TypeScript

- 4-space indentation, 100-column limit, no trailing whitespace.
- Same commenting style as C: short purpose annotations at block
  level, not per line.

### General

- Read existing files before writing code; prefer editing over
  rewriting whole files.
- Test changes before declaring them done (`make test` and/or the e2e
  suite).
- Commit messages follow Conventional Commits (`feat:`, `fix:`,
  `chore:`), imperative and lowercase.
- The only trailer is `Co-Authored-By`. Never add session URLs or
  other trailers to commit messages.
- Keep solutions simple and direct.

## Documentation

Update the relevant doc when behavior changes:

- Protocol, wire format, crypto, API: [docs/SPEC.md](docs/SPEC.md)
- Build, dependencies, testing: [docs/DEV.md](docs/DEV.md)
- Deployment / self-hosting: [docs/DEPLOY.md](docs/DEPLOY.md)
- User-facing CLI usage: [README.md](README.md)

Keep the zh-CN and ja translations under `docs/i18n/` in sync with
any English doc you change. The Japanese SPEC.md uses だ・である調
(plain form); CJK paragraphs must not be hard-wrapped mid-sentence
(a CJK-CJK line break renders as a stray space).

Wording: all instances are peers. Avoid "official instance" or
"third-party instance". Refer to `shyake.eee.coffee` by name and
describe it as the client's built-in fallback (used only when no
instance is configured).
