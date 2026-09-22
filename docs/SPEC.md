## Shyake Technical Specification

Copyright (c) 2026 Salmonization. BSD 2-Clause License.

<table>
<tr><td>Version</td><td>0.2</td></tr>
<tr><td>Last updated</td><td>2026-07-11</td></tr>
</table>

---

### 1. Overview

Shyake is a post-quantum, end-to-end encrypted asynchronous mail
system with a POSIX-style command-line interface client, designed as
a decentralized communication method to resist censorship and
surveillance.

Key properties:

- **End-to-end encryption**: the server never holds plaintext. All
  message content is encrypted client-side before transmission.
- **Post-quantum cryptography**: key encapsulation uses ML-KEM-768;
  authentication uses ML-DSA-65 (CRYSTALS-Dilithium), both from
  [liboqs](https://github.com/open-quantum-safe/liboqs).
- **Decentralized**: any operator can host their own instance with
  almost zero cost. Instances optionally federate using a
  server-to-server relay model.
- **Stateless server**: the server stores only ciphertext and public
  keys.
- **Encrypted keys at rest**: client secret keys are optionally
  protected on disk with a passphrase (scrypt + ChaCha20-Poly1305).

---

### 2. Architecture

#### 2.1 Components

```
shyake/
├── client/                 # C client
│   ├── src/lib/            # core logic (network, crypto, mail,
│   │                       #   account, passphrase)
│   ├── src/cli/            # CLI parsing, display, prompt, config,
│   │                       #   drafts, self-update
│   ├── include/shyake.h    # public API (opaque pointer)
│   ├── tests/              # library test programs
│   └── Makefile
├── server/
│   └── cf/                 # Cloudflare Worker
│       ├── src/index.ts    # Hono routes
│       ├── src/utils.ts    # helpers (PoW, username validation)
│       ├── migrations/     # D1 schema migrations
│       └── wrangler.toml   # Worker configuration
└── docs/
```

#### 2.2 Client

- **Standard**: C11, POSIX.1-2008 (`_POSIX_C_SOURCE=200809L`)
- **Build system**: GNU Make; cross-platform (macOS, GNU/Linux, Termux)
- **Artifacts**:
  - `bin/shyake`: CLI binary, statically linked against `libshyake.a`
  - `lib/libshyake.a`: static library
  - `lib/libshyake.so` / `libshyake.dylib`: shared library for FFI
- **Dependencies**:
  - `liboqs` (always linked statically): ML-KEM and ML-DSA
  - `libcurl`: HTTP transport
  - `libcrypto` (OpenSSL): SHA-256 fingerprints, SHA-1 (PoW),
    ChaCha20-Poly1305 AEAD, scrypt KDF (`EVP_PBE_scrypt`)
  - `cJSON` (vendored): JSON parsing

#### 2.3 Server

- **Runtime**: Cloudflare Workers
- **Framework**: [Hono](https://hono.dev/)
- **Database**: Cloudflare D1 (SQLite)
- **Signature verification**: ML-DSA-65 compiled to WebAssembly
  (`mldsa65-wasm`), loaded via the Wrangler `CompiledWasm` rule.

---

### 3. Cryptographic Design

#### 3.1 Key Pairs

Each user generates two independent key pairs locally via `liboqs`:

| Purpose | Algorithm | Files |
|---|---|---|
| Key encapsulation | ML-KEM-768 | `kem_pk.bin`, `kem_sk.bin` |
| Authentication / signing | ML-DSA-65 | `sig_pk.bin`, `sig_sk.bin` |

Public keys are stored as raw bytes and uploaded to the server on
registration. Secret keys are stored in the encrypted-at-rest format
described in §3.7 when a passphrase is set, or as raw bytes when no
passphrase is set.

#### 3.2 Message Encryption

1. Generate a random 256-bit symmetric key.
2. Encrypt `subject` and `body` with **ChaCha20-Poly1305** using that
   key, each with its own random 96-bit nonce. Each ciphertext is
   transmitted as `base64(nonce || ciphertext || tag)`.
3. Encapsulate to the **recipient's ML-KEM public key**: KEM
   encapsulation yields a KEM ciphertext and a 32-byte shared secret;
   the symmetric key is XORed with the shared secret and appended:
   `enc_key_recipient = base64(kem_ct || (sym_key XOR ss))`.
4. Repeat the encapsulation with the **sender's own ML-KEM public
   key** → `enc_key_sender` (allows the sender to read their sent
   box).

Decryption reverses the process: the client decapsulates the shared
secret with its KEM secret key, XORs it against the encrypted key
field to recover the symmetric key, then decrypts the content.

The standalone file encryption commands (`enc` / `dec`) use the same
ML-KEM-768 + ChaCha20-Poly1305 construction with a length-prefixed
binary container (`.enc` file).

#### 3.3 Authentication Protocol

All authenticated operations are signed with ML-DSA-65. Two carriage
forms are used:

**Header-based** (all authenticated endpoints except registration and
mail submission):

```
X-Shyake-Username:  <username>
X-Shyake-Timestamp: <unix seconds>
X-Shyake-Signature: <base64(ML-DSA-65 signature)>
X-Shyake-Pow:       <Hashcash token>
```

The signed message is a deterministic string constructed from the
HTTP method, endpoint (including the query string), username, and
timestamp. For example:

```
GET:/api/mail?type=inbox:salmon:1749513600
```

**Body-based** (`POST /api/register` and `POST /api/mail`): the
signature and PoW token travel as JSON fields of the request body.
The signed message is the compact JSON serialization of the payload
subset below (field order as produced by the client):

`POST /api/register`:

```json
{
  "username": "...",
  "kem_pubkey": "...",
  "sig_pubkey": "...",
  "timestamp": "1749513600"
}
```

`POST /api/mail`:

```json
{
  "sender": "...",
  "recipient": "...",
  "recipient_kem_fingerprint": "...",
  "enc_subject": "...",
  "enc_body": "...",
  "timestamp": "1749513600",
  "size": 512
}
```

The full request body additionally carries `enc_key_sender`,
`enc_key_recipient`, `signature`, and `pow`, which are not part of
the signed subset.

The server verifies the signature using the sender's `sig_pubkey`
stored in D1 (or fetched from the sender's instance for federated
mail), via the WASM ML-DSA module.

#### 3.4 Anti-Replay

A timestamp is included in every signed message. The server rejects
requests whose timestamp deviates from server time by more than
**300 seconds (5 minutes)**.

#### 3.5 Proof of Work

Every authenticated request, including reads, requires a
Hashcash-v1-style PoW token with a **20-bit** SHA-1 difficulty:

```
1:<bits>:<yymmdd>:<resource>::<rand>:<counter-hex>
```

`resource` is the acting username. The token is minted client-side
and verified server-side before signature verification or any
database work is performed.

#### 3.6 Key Fingerprint

A fingerprint is the lowercase hex-encoded **SHA-256** of the raw
(decoded) ML-KEM public key bytes. The client caches trusted keys in
`~/.config/shyake/known_hosts`, one space-separated entry per line:

```
<username> <fingerprint-hex> <kem_pubkey-base64>
```

The client compares the recipient's live key against `known_hosts`
before every send, and additionally embeds
`recipient_kem_fingerprint` in the payload so the server can compare
it against the stored key and reject stale sends with `KEY_MISMATCH`
(HTTP 409).

#### 3.7 Secret Key Protection at Rest

When the user sets a non-empty passphrase, secret key files
(`kem_sk.bin`, `sig_sk.bin`) are written in the `SHYK` container
format:

| Offset | Size | Field |
|---|---|---|
| 0 | 4 B | magic `"SHYK"` |
| 4 | 1 B | version `0x01` |
| 5 | 1 B | KDF id `0x01` (scrypt) |
| 6 | 32 B | salt (random) |
| 38 | 4 B | scrypt `N` (LE u32, default 65536) |
| 42 | 4 B | scrypt `r` (LE u32, default 8) |
| 46 | 4 B | scrypt `p` (LE u32, default 1) |
| 50 | 12 B | ChaCha20-Poly1305 nonce (random) |
| 62 | — | ciphertext (same length as the plaintext key) |
| end | 16 B | Poly1305 tag |

The 62-byte header is bound as AAD, so any tampering with the KDF
parameters fails authentication. The KDF derives a 256-bit
ChaCha20-Poly1305 key from the passphrase.

Files without the `SHYK` magic are treated as legacy raw keys and
loaded as-is. An empty passphrase writes raw (unencrypted) keys.

The passphrase is prompted interactively (terminal echo disabled),
or supplied via the `SHYAKE_PASSPHRASE` environment variable for
non-interactive use. `rotate` prompts for the current passphrase and
a new one; the new key pairs are saved under the new passphrase only
after the server confirms the rotation.

#### 3.8 Local Encrypted Drafts

`shyake compose` stores drafts in `drafts/<id>.json` inside the
config directory. Drafts never touch
the server. Each draft uses the same hybrid scheme as mail (§3.2):
a random 32-byte symmetric key encrypts each field with
ChaCha20-Poly1305, and the key is ML-KEM-768-encapsulated to the
user's own KEM public key.

```json
{
  "version": 1,
  "draft_id": "3",
  "created": 1752400000,
  "modified": 1752400000,
  "size": 123,
  "enc_key": "<b64: kem_ct || (sym_key XOR ss)>",
  "enc_recipient": "<b64: nonce||ct||mac>",
  "enc_subject": "<b64: nonce||ct||mac>",
  "enc_body": "<b64: nonce||ct||mac>"
}
```

Recipient, subject, and body are all encrypted at rest; only
timestamps, size, and the id are plaintext. An empty
`enc_recipient` / `enc_subject` string denotes an empty field.

Because saving only needs the public key, `compose` requires no
passphrase; listing, reading, editing, and sending a draft require
unlocking the KEM secret key. Draft ids are small integers allocated
locally (max existing id + 1, created with `O_EXCL`).

The compose editor works on a plaintext temp file created with
`mkstemp` (mode 0600) inside the config directory, never `/tmp`.
The file is zero-overwritten and unlinked afterwards. When the
editor is `vim`/`nvim`, it is invoked with `-n -i NONE` so no
plaintext leaks into swap or viminfo files.

---

### 4. Database Schema

Managed by Cloudflare D1 (SQLite). Migration:
`migrations/0001_initial.sql`.

#### `users`

| Column | Type | Notes |
|---|---|---|
| `username` | TEXT PK | Regex `^(?=.*[a-zA-Z])[a-zA-Z0-9_]{4,16}$` |
| `kem_pubkey` | TEXT | Base64-encoded ML-KEM-768 public key |
| `sig_pubkey` | TEXT | Base64-encoded ML-DSA-65 public key |
| `created_at` | INTEGER | UNIX timestamp |

On `destroy`, `kem_pubkey` and `sig_pubkey` are set to empty strings
and all mail and block rows involving the user are deleted. The user
row itself is **retained** to permanently lock the username.

#### `mail`

| Column | Type | Notes |
|---|---|---|
| `mail_id` | TEXT PK | 10-char base58 string, server-assigned |
| `sender` | TEXT | Local name or `user@domain` for federated |
| `recipient` | TEXT | Local name or `user@domain` for federated |
| `enc_key_sender` | TEXT | KEM-encapsulated key for sender |
| `enc_key_recipient` | TEXT | KEM-encapsulated key for recipient |
| `enc_subject` | TEXT | ChaCha20-Poly1305 ciphertext, base64 |
| `enc_body` | TEXT | ChaCha20-Poly1305 ciphertext, base64 |
| `size` | INTEGER | Plaintext body byte count (UI display only) |
| `signature` | TEXT | Sender's ML-DSA-65 signature, base64 |
| `timestamp` | INTEGER | Server-assigned UNIX timestamp |

Local addresses are stored bare: an `@<INSTANCE_DOMAIN>` suffix is
stripped before insertion. Indexes on `recipient` and `sender` serve
the mailbox queries. `rotate` deletes all mail rows where the user is
sender or recipient.

#### `blocks`

| Column | Type | Notes |
|---|---|---|
| `blocker` | TEXT | Username of the blocking user |
| `blocked` | TEXT | Username, `user@domain`, or bare domain |
| `created_at` | INTEGER | UNIX timestamp |
| PK | | `(blocker, blocked)` composite |

On mail submission the server rejects the send with HTTP 403 if the
recipient has blocked the sender's local name or the sender's domain.

---

### 5. HTTP API

All endpoints are hosted on the Cloudflare Worker. Base URL is the
configured `INSTANCE_DOMAIN`.

#### 5.1 Public Endpoints

| Method | Path | Description |
|---|---|---|
| `GET` | `/health` | Liveness check; queries D1 |
| `GET` | `/api/pubkey/:username` | Return `kem_pubkey`, `sig_pubkey` |
| `GET` | `/api/client/version` | Latest client release tags |

`/api/pubkey/:username` supports the `user@domain` syntax; if the
domain differs from the local instance, the server proxies the
request to the remote instance (requires federation enabled).

`/api/client/version` proxies the GitHub Releases API and returns
`{"release": "vX.Y.Z", "pre_release": "vX.Y.Z-..."}` (either field
may be absent). Results are cached in KV for one hour. See §12.

#### 5.2 Authenticated Endpoints

All verify a PoW token, timestamp window, and ML-DSA-65 signature
(§3.3). `POST /api/register` and `POST /api/mail` carry the auth
fields in the JSON body; all others use the `X-Shyake-*` headers.

| Method | Path | Description |
|---|---|---|
| `POST` | `/api/register` | Register a new user |
| `POST` | `/api/mail` | Send a mail |
| `GET` | `/api/mail?type=inbox\|sent` | List mailbox metadata |
| `GET` | `/api/mail/:id` | Fetch a single mail (full ciphertext) |
| `DELETE` | `/api/mail/:id` | Burn (delete) a mail |
| `POST` | `/api/block` | Block a user or domain |
| `DELETE` | `/api/block` | Unblock a user or domain |
| `GET` | `/api/block` | List the caller's blocks |
| `POST` | `/api/rotate` | Rotate public keys |
| `DELETE` | `/api/destroy` | Destroy account |

Notable status codes on `POST /api/mail`: `409` (`KEY_MISMATCH`, the
supplied recipient fingerprint no longer matches), `410`
(`USER_DESTROYED`), `413` (payload too large), `403` (blocked, bad
PoW, or stale timestamp).

#### 5.3 Size Limit

The server enforces a hard cap on the raw HTTP request body of `POST
/api/mail`. The default is **196608 bytes (192 KiB)**, configurable
in `wrangler.toml` via `MAX_MAIL_SIZE`. The absolute ceiling is
786432 bytes (768 KiB), imposed by Cloudflare D1's single-row limit.

---

### 6. Federation

#### 6.1 Addressing

- **Local user**: `username` (no `@`)
- **Remote user**: `username@instance.domain`

The client always communicates only with the user's own instance.
When sending to a remote recipient, the client qualifies its own
sender string as `username@<own-domain>`.

#### 6.2 Outbound Mail Relay

When `recipient` belongs to a remote instance:

1. The client posts the signed, encrypted payload to the **sender's
   own instance** (`POST /api/mail`).
2. The sender's instance stores the mail in its local D1 database.
3. In the same request lifecycle (via `executionCtx.waitUntil`), the
   server forwards the original raw payload to
   `https://<recipientDomain>/api/mail`.

The recipient's instance independently verifies the sender's
signature by fetching the sender's public key from the sender's
instance (`GET /api/pubkey/<sender>`).

Both the sender's and recipient's databases store the mail. This
ensures atomicity for the sender (sent-box availability) regardless
of remote instance availability.

#### 6.3 Federation Toggle

Configurable via `FEDERATION_ENABLED` in `wrangler.toml`. When
`false`, the instance refuses to resolve remote users, which rejects
both incoming relayed mail and outgoing cross-instance sends.

---

### 7. Trust Model (TOFU + OOB)

Shyake uses **Trust On First Use (TOFU)** for public key management:

- **First contact**: the client queries
  `GET /api/pubkey/<recipient>`, computes the KEM fingerprint, and
  silently appends it to `~/.config/shyake/known_hosts`.
- **Subsequent contacts**: before every send, the fetched key is
  compared against the `known_hosts` entry; a mismatch aborts
  locally with `KEY_MISMATCH` before anything is transmitted.
- **Server-side double check**: the payload embeds
  `recipient_kem_fingerprint`; the server independently rejects with
  HTTP 409 if it no longer matches the stored key.
- **Key rotation detected**: the client prints a fatal error and
  halts:

```
FATAL: Remote public key of recipient has changed!
RUN 'shyake fingerprint <username>' to inspect and update trust.
```

The `fingerprint` command provides **out-of-band (OOB)
verification**: it fetches the current public key from the server,
computes the fingerprint, and compares it against `known_hosts`.
Output shows GPG-style hex groups plus an OpenSSH-style randomart
image. The `--update` flag rewrites `known_hosts` after the user
verifies the new fingerprint through a trusted channel.

---

### 8. Client Library ABI

`libshyake` is the protocol implementation, and the bundled CLI is
only a reference client built on top of it. The library contains
exclusively core, universally applicable logic: cryptography,
wire-format encoding, and the send/receive operations in this spec.
Client-specific concerns (argument parsing, display, prompts,
self-update) live in `src/cli/` and must not migrate into the
library. Third-party developers can build fully protocol-compatible
clients (TUI, GUI, or any language via FFI) on `libshyake` alone.

The core library exposes a stable C API through `include/shyake.h`.
Internal state is hidden behind an opaque pointer to prevent ABI
breakage:

```c
typedef struct shyake_ctx shyake_ctx;

shyake_ctx* shyake_init_ctx(const shyake_config *config);
void        shyake_free_ctx(shyake_ctx *ctx);

/* passphrase for secret key files (§3.7) */
void shyake_set_passphrase(shyake_ctx *ctx, const char *pp);
void shyake_set_new_passphrase(shyake_ctx *ctx, const char *pp);

/* detail of the last failure on this ctx, "" if none */
const char* shyake_last_error(shyake_ctx *ctx);
```

Internal struct definitions live in `src/lib/lib_internal.h`, not
exposed to callers. The library never writes to stdout/stderr: on
failure it records a human-readable detail retrievable via
`shyake_last_error(ctx)` (valid until the next call on the same
context) and returns a semantic error code. Error codes are a typed
enum (`shyake_err`), with `SHYAKE_OK = 0` for backward compatibility:

| Code | Meaning |
|---|---|
| `SHYAKE_OK` | Success |
| `SHYAKE_ERR` | Generic / internal failure |
| `SHYAKE_ERR_NETWORK` | libcurl transport failure |
| `SHYAKE_ERR_HTTP` | Unexpected HTTP status |
| `SHYAKE_ERR_KEY_MISMATCH` | HTTP 409: recipient key rotated |
| `SHYAKE_ERR_GONE` | HTTP 410: recipient destroyed |
| `SHYAKE_ERR_NOT_FOUND` | HTTP 404 |
| `SHYAKE_ERR_FORBIDDEN` | HTTP 403 |
| `SHYAKE_ERR_CRYPTO` | Cryptographic operation failed |
| `SHYAKE_ERR_NO_INSTANCE` | Instance URL not configured |

API groups: context lifecycle, key generation, PoW minting,
registration, mail (`shyake_send`, `shyake_check`, `shyake_fetch`,
`shyake_check_one`, `shyake_burn`), local saved mail
(`shyake_save_mail`, `shyake_read_saved`, `shyake_check_saved_one`,
`shyake_list_saved`), account (`shyake_block`, `shyake_list_blocks`,
`shyake_rotate`, `shyake_destroy`), fingerprints
(`shyake_fingerprint`), self-encryption primitives
(`shyake_selfenc_begin`, `shyake_selfdec_new`, `shyake_selfdec_key`,
`shyake_selfdec_free`, `shyake_seal_b64`, `shyake_unseal_b64`), and
standalone file encryption (`shyake_enc_file`, `shyake_dec_file`).

Drafts and self-update are CLI-layer features (`src/cli/`), not part
of the library API. The drafts on-disk format (§3.8) is built
entirely on the public self-encryption primitives; other clients may
reuse the format or store drafts their own way.

The shared library (`libshyake.so` / `libshyake.dylib`) is intended
for third-party FFI consumers. The CLI binary links against the
static archive (`libshyake.a`) for single-file distribution.

---

### 9. Local Configuration

Configuration directory: `~/.config/shyake/` (default) or a custom
path specified with `-c` / `--config`.

| File | Content |
|---|---|
| `config` | Shell-style key=value settings |
| `kem_pk.bin` / `sig_pk.bin` | Public keys (raw bytes) |
| `kem_sk.bin` / `sig_sk.bin` | Secret keys (raw or `SHYK`, §3.7) |
| `known_hosts` | `username fingerprint kem_pubkey` per line |
| `saved/<id>.json` | Encrypted mail saved by `shyake save` |
| `drafts/<id>.json` | Encrypted drafts written by `shyake compose` (§3.8) |

`saved/<id>.json` is the verbatim ciphertext JSON returned by
`GET /api/mail/:id`; it is decrypted only on `shyake read`.

Key `config` fields:

| Key | Default | Description |
|---|---|---|
| `INSTANCE` | — | Instance base URL |
| `USERNAME` | — | Registered username (set by `register`) |
| `TIME_FORMAT` | `%Y-%m-%d %H:%M` | `strftime` format |
| `TIME_FORMAT_RECENT` | — | Format for mail < 180 days old |
| `TIME_ZONE` | `auto` | Integer hour offset or `auto` |
| `CHECK_COLUMNS` | `id,sender,subject,size,date` | `check` layout |
| `NO_COLOR` | `0` | Set `1` to disable ANSI colors |
| `DEFAULT_ACTION` | `0` | 0=man, 1=check inbox, 2=inbox --count |
| `EDITOR` | — | Editor for `compose` (falls back to `$VISUAL`, `$EDITOR`, then `ed`) |

Recognized environment variables:

| Variable | Effect |
|---|---|
| `SHYAKE_PASSPHRASE` | Supplies the key passphrase non-interactively |
| `NO_COLOR` | Disables ANSI colors (any non-empty value) |

---

### 10. CLI Reference

#### Global Options

| Flag | Description |
|---|---|
| `-c, --config <dir>` | Use alternate config directory |
| `--plain` | Disable pager, colors, and truncation |
| `--no-color` | Disable ANSI color output |
| `--debug` | Verbose curl logs to stderr |

#### Commands

| Command | Description |
|---|---|
| `init [-c <dir>]` | Generate config directory and key pairs |
| `register -u <user> -i <url>` | Register on an instance |
| `whoami` | Print current profile (no network) |
| `send -t <to> [-s <subj>] [file]` | Send a mail (text only) |
| `send --draft <id> [-t <to>] [-s <subj>]` | Send a stored draft (deleted on success) |
| `compose [<id>]` | Compose or edit an encrypted draft (§3.8) |
| `check inbox\|sent [opts]` | List mailbox metadata |
| `check <id>` | Inspect a single mail header |
| `check saved [<id>]` | List / inspect locally saved mail |
| `check drafts [<id>]` | List drafts / inspect a draft header |
| `fetch [-r] <id>` | Decrypt and print a mail |
| `save <id>` | Store encrypted mail locally |
| `read [-r] <id>` | Decrypt and print a saved mail |
| `read [-r] drafts <id>` | Decrypt and print a draft |
| `burn <id>` | Delete a mail (sender or recipient) |
| `block <target>` | Block a user or domain |
| `unblock <target>` | Unblock a user or domain |
| `blocklist` | List blocked users and domains |
| `rotate` | Rotate key pairs (clears all own mail) |
| `fingerprint [<user>] [--update]` | Compare key fingerprints |
| `destroy` | Destroy account and local config |
| `enc <file> [-t <user>] [-o <out>]` | Encrypt a standalone file |
| `dec <file> [-o <out>]` | Decrypt a standalone file |
| `update [stable\|preview]` | Show versions / self-update |
| `man [<command>]` | Display documentation |
| `version` | Print version string |

`check inbox|sent` accepts `--count`, `--json`, `--csv`, and
`--no-header`. `send` recipients may be local (`username`) or remote
(`username@instance`); binary data must be base64-encoded by the
caller. `enc`/`dec` are intended for debugging and testing.

---

### 11. Worker Configuration (`wrangler.toml`)

| Variable | Default | Description |
|---|---|---|
| `INSTANCE_DOMAIN` | — | Canonical domain of this instance |
| `REGISTRATION_ENABLED` | `true` | Accept new user registrations |
| `RESERVED_USERNAMES` | `admin,system,...` | Reserved names (CSV) |
| `FEDERATION_ENABLED` | `true` | Accept and relay federated mail |
| `MAX_MAIL_SIZE` | `196608` | Max payload bytes, `POST /api/mail` |

Required bindings:

| Binding | Type | Purpose |
|---|---|---|
| `DB` | D1 database | Users, mail, blocks (§4) |
| `VERSION_CACHE` | KV namespace | Release lookup cache (§12) |

A `CompiledWasm` build rule loads the `mldsa65-wasm` module.

---

### 12. Release Channels & Self-Update

Releases are published on GitHub in two channels: **stable** (normal
releases) and **preview** (pre-releases). The server endpoint
`GET /api/client/version` proxies the GitHub Releases API, picks the
newest tag of each channel, and caches the result in KV for one
hour.

`shyake update` fetches this endpoint from the **user's own
instance** (`INSTANCE` in the profile config), so every instance
relays the GitHub API with its own KV cache; `shyake.eee.coffee` is
only a built-in fallback used when no instance is configured. Tags
are compared using semver
ordering (`vX.Y.Z`; a release outranks a pre-release of the same
base version). The preview channel is offered only when it is newer
than stable.

`shyake update stable|preview` performs the self-update:

1. Download the OS/arch-matched release asset
   (`shyake-<os>-<arch>.tar.gz`) and `sha256sums.txt` from GitHub
   Releases.
2. Verify the archive's SHA-256 against the checksum file; abort on
   mismatch.
3. Extract the archive and replace the running binary in place
   (resolved via `/proc/self/exe`, `_NSGetExecutablePath`, or
   `which shyake`).
