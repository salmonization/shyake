# shyake GUI

An experimental local web GUI for shyake, built on `libshyake` through
Bun's FFI (`bun:ffi`). It shares the CLI's config directory
(`~/.config/shyake`), so the same account works in both clients.

## Requirements

- [Bun](https://bun.sh) 1.x
- `libshyake` built: `cd ../client && make`

## Run

```sh
bun run start
```

This starts a local server on <http://127.0.0.1:8788> and opens it in
your browser.

Environment variables:

- `SHYAKE_CONFIG` — use an alternative config directory (same as the
  CLI's `-c`)
- `SHYAKE_GUI_PORT` — listen port (default 8788)
- `SHYAKE_GUI_NO_OPEN=1` — do not auto-open the browser
- `SHYAKE_LIB` — override the path to `libshyake.{dylib,so}`

## Architecture

- `src/ffi.ts` — `bun:ffi` bindings for the public C API in
  `client/include/shyake.h`. The structs are mirrored by hand in their
  LP64 layout; all library calls on a session are serialized.
- `src/server.ts` — dependency-free HTTP API on `Bun.serve`, bound to
  `127.0.0.1` only. Every `/api` route requires a per-launch random
  token that is injected into the served page, so other local
  processes and websites cannot drive the account. The passphrase
  lives only inside the library context and is never persisted.
- `public/` — vanilla HTML/CSS/JS UI, no build step. Views: setup &
  register → unlock → inbox / sent / saved / compose / account
  (fingerprint, block list, key rotation, account destruction).
