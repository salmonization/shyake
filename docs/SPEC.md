## Shyake — Technical Specification

Copyright (c) 2026 Salmonization. BSD 2-Clause License.

<table>
<tr><td>Version</td><td>0.1</td></tr>
<tr><td>Last updated</td><td>2026-06-10</td></tr>
</table>

---

### 1. Overview

Shyake is a post-quantum, end-to-end encrypted asynchronous mail system
with a POSIX-style command-line interface client, designed as a
decentralized communication method to resist censorship and surveillance.

Key properties:

- **End-to-end encryption**: the server never holds plaintext. All
  message content is encrypted client-side before transmission.
- **Post-quantum cryptography**: key encapsulation uses ML-KEM-768;
  authentication uses ML-DSA-65 (CRYSTALS-Dilithium), both from
  [liboqs](https://github.com/open-quantum-safe/liboqs).
- **Decentralized**: any operator can host their own instance with almost
  zero cost. Instances optionally federate using a server-to-server relay
  model.
- **Stateless server**: the server stores only ciphertext and public keys.

---

### 2. Architecture

#### 2.1 Components

```
shyake/
├── client/                 # C client
│   ├── src/lib/            # core logic (network, crypto, mail)
│   ├── src/cli/            # CLI parsing, display, config
│   ├── include/shyake.h    # public API (opaque pointer)
│   └── Makefile
├── server/                 # Cloudflare Worker
│   ├── src/index.ts        # Hono routes
│   ├── src/utils.ts        # helpers (PoW, username validation)
│   ├── migrations/         # D1 schema migrations
│   └── wrangler.toml       # Worker configuration
└── docs/
```

#### 2.2 Client

- **Standard**: C11, POSIX.1-2008 (`_POSIX_C_SOURCE=200809L`)
- **Build system**: GNU Make; cross-platform (macOS, GNU/Linux, Termux)
- **Artifacts**:
  - `bin/shyake` — CLI binary, statically linked against
    `libshyake.a`
  - `lib/libshyake.a` — static library
  - `lib/libshyake.so` / `libshyake.dylib` — shared library for FFI
- **Dependencies**:
  - `liboqs` (always linked statically) — ML-KEM and ML-DSA
  - `libcurl` — HTTP transport
  - `libcrypto` (OpenSSL) — SHA-256 fingerprint computation,
    ChaCha20-Poly1305 symmetric encryption
  - `cJSON` (vendored) — JSON parsing

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

Both public keys are uploaded to the server on registration.

#### 3.2 Message Encryption

1. Generate a random 256-bit symmetric key.
2. Encrypt `subject` and `body` independently with
   **ChaCha20-Poly1305** using that key.
3. Encapsulate the symmetric key with the **recipient's ML-KEM
   public key** → `enc_key_recipient`.
4. Encapsulate the same key with the **sender's own ML-KEM public
   key** → `enc_key_sender` (allows the sender to read their sent
   box).
5. All ciphertext is base64-encoded before storage.

Decryption reverses the process: the client uses its KEM private key
to decapsulate the appropriate encapsulated key, then decrypts the
content.

#### 3.3 Authentication Protocol

All mutating operations and authenticated reads use ML-DSA-65
signatures carried in HTTP headers:

```
X-Shyake-Username:  <username>
X-Shyake-Timestamp: <unix seconds>
X-Shyake-Signature: <base64(ML-DSA-65 signature)>
X-Shyake-Pow:       <Hashcash token>
```

The signed message is a deterministic string constructed from the
HTTP method, endpoint, username, and timestamp — e.g.:

```
GET:/api/mail?type=inbox:salmon:1749513600
```

For `POST /api/mail`, the signed payload is a JSON object containing:

```json
{
  "sender": "...",
  "recipient": "...",
  "recipient_kem_fingerprint": "...",
  "enc_subject": "...",
  "enc_body": "...",
  "timestamp": 1749513600,
  "size": 512
}
```

The server verifies the signature using the sender's `sig_pubkey`
stored in D1, via the WASM ML-DSA module.

#### 3.4 Anti-Replay

The `timestamp` field is included in every signed payload. The server
rejects requests whose timestamp deviates from server time by more
than **300 seconds (5 minutes)**.

#### 3.5 Proof of Work

All write operations require a Hashcash-style PoW token with a
**20-bit** difficulty. The token is computed client-side and verified
server-side before any database or crypto work is performed.

#### 3.6 Key Fingerprint

A fingerprint is the lowercase hex-encoded **SHA-256** of the raw
(decoded) ML-KEM public key bytes. The client stores fingerprints in
`~/.config/shyake/known_hosts`. The server also computes and compares
the recipient's stored KEM fingerprint on each incoming `POST /api/mail`
to detect key rotation.

---

### 4. Database Schema

Managed by Cloudflare D1 (SQLite). Migration: `migrations/0001_initial.sql`.

#### `users`

| Column | Type | Notes |
|---|---|---|
| `username` | TEXT PK | Regex `^(?=.*[a-zA-Z])[a-zA-Z0-9_]{4,16}$` |
| `kem_pubkey` | TEXT | Base64-encoded ML-KEM-768 public key |
| `sig_pubkey` | TEXT | Base64-encoded ML-DSA-65 public key |
| `created_at` | INTEGER | UNIX timestamp |

On `destroy`, `kem_pubkey` and `sig_pubkey` are set to empty strings.
The row is **retained** to permanently lock the username.

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
| `size` | INTEGER | Plaintext byte count (for UI display only) |
| `signature` | TEXT | Sender's ML-DSA-65 signature, base64 |
| `timestamp` | INTEGER | Server-assigned UNIX timestamp |

Indexes on `recipient` and `sender` for efficient mailbox queries.

#### `blocks`

| Column | Type | Notes |
|---|---|---|
| `blocker` | TEXT | Username of the blocking user |
| `blocked` | TEXT | Username, `user@domain`, or bare domain |
| `created_at` | INTEGER | UNIX timestamp |
| PK | | `(blocker, blocked)` composite |

---

### 5. HTTP API

All endpoints are hosted on the Cloudflare Worker. Base URL is the
configured `INSTANCE_DOMAIN`.

#### 5.1 Public Endpoints

| Method | Path | Description |
|---|---|---|
| `GET` | `/health` | Liveness check; queries D1 |
| `GET` | `/api/pubkey/:username` | Return `kem_pubkey` and `sig_pubkey` |

`/api/pubkey/:username` supports the `user@domain` syntax; if the
domain differs from the local instance, the server proxies the
request to the remote instance (requires federation enabled).

#### 5.2 Authenticated Endpoints

All require the four headers listed in §3.3.

| Method | Path | Description |
|---|---|---|
| `POST` | `/api/register` | Register a new user |
| `POST` | `/api/mail` | Send a mail |
| `GET` | `/api/mail?type=inbox\|sent` | List mailbox metadata |
| `GET` | `/api/mail/:id` | Fetch a single mail (full ciphertext) |
| `DELETE` | `/api/mail/:id` | Burn (delete) a mail |
| `POST` | `/api/block` | Block a user or domain |
| `DELETE` | `/api/block` | Unblock a user or domain |
| `POST` | `/api/rotate` | Rotate public keys |
| `DELETE` | `/api/destroy` | Destroy account |

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

#### 6.2 Outbound Mail Relay

When `recipient` belongs to a remote instance:

1. The client posts the signed, encrypted payload to the **sender's
   own instance** (`POST /api/mail`).
2. The sender's instance stores the mail in its local D1 database.
3. In the same request lifecycle (via `executionCtx.waitUntil`), the
   server forwards the original raw payload to
   `https://<recipientDomain>/api/mail`.

The recipient's instance independently verifies the sender's signature
by fetching the sender's public key from the sender's instance
(`GET /api/pubkey/<sender>`).

Both the sender's and recipient's databases store the mail. This
ensures atomicity for the sender (sent-box availability) regardless
of remote instance availability.

#### 6.3 Federation Toggle

Configurable via `FEDERATION_ENABLED` in `wrangler.toml`. When
`false`, the instance rejects both incoming relayed mail and outgoing
cross-instance sends.

---

### 7. Trust Model (TOFU + OOB)

Shyake uses **Trust On First Use (TOFU)** for public key management:

- **First contact**: the client queries `GET /api/pubkey/<recipient>`,
  computes the KEM fingerprint, and silently appends it to
  `~/.config/shyake/known_hosts`.
- **Subsequent contacts**: the cached public key in `known_hosts` is
  used directly without a network request.
- **Key rotation detected**: if the server returns `KEY_MISMATCH`
  (HTTP 409), the client prints a fatal error and halts:

```
FATAL: Remote public key of recipient has changed!
RUN 'shyake fingerprint <username>' to inspect and update trust.
```

The `fingerprint` command provides **out-of-band (OOB) verification**:
it fetches the current public key from the server, computes the
fingerprint, and compares it against `known_hosts`. Output follows
GPG-style hex groups. The `--update` flag rewrites `known_hosts`
after the user verifies the new fingerprint through a trusted channel.

---

### 8. Client Library ABI

The core library exposes a stable C API through `include/shyake.h`.
Internal state is hidden behind an opaque pointer to prevent ABI
breakage:

```c
typedef struct shyake_ctx shyake_ctx;

shyake_ctx* shyake_init_ctx(const shyake_config *config);
void        shyake_free_ctx(shyake_ctx *ctx);
```

Internal struct definition lives in `src/lib/internal.h`, not
exposed to callers. Semantic error codes are returned as a typed
enum (`shyake_err`), with `SHYAKE_OK = 0` for backward compatibility.

The shared library (`libshyake.so` / `libshyake.dylib`) is intended
for third-party FFI consumers. The CLI binary links against the
static archive (`libshyake.a`) for single-file distribution.

---

### 9. Local Configuration

Configuration directory: `~/.config/shyake/` (default) or a custom
path specified with `-c`.

| File | Content |
|---|---|
| `config` | Shell-style key=value settings |
| `username` | Registered username (written on successful register) |
| `kem_pk.bin` / `kem_sk.bin` | ML-KEM-768 key pair (raw bytes) |
| `sig_pk.bin` / `sig_sk.bin` | ML-DSA-65 key pair (raw bytes) |
| `known_hosts` | Tab-separated: `username<TAB>fingerprint<TAB>kem_pubkey` |

Key `config` fields:

| Key | Default | Description |
|---|---|---|
| `INSTANCE` | — | Instance base URL |
| `TIME_FORMAT` | `%Y-%m-%d %H:%M` | `strftime` format for timestamps |
| `TIME_ZONE` | `auto` | Integer hour offset or `auto` |
| `CHECK_COLUMNS` | `id,sender,subject,size,date` | Column layout for `check` |
| `NO_COLOR` | `0` | Set `1` to disable ANSI colors |
| `DEFAULT_ACTION` | `0` | 0=man, 1=check inbox, 2=check inbox --count |

---

### 10. CLI Reference

#### Global Options

| Flag | Description |
|---|---|
| `--plain` | Disable pager, colors, and truncation; use tab separators |
| `--no-color` | Disable ANSI color output (also respected via `NO_COLOR=1`) |
| `--debug` | Print verbose curl handshake info and internal vars to stderr |
| `-c <dir>` | Use alternate config directory |

#### Commands

| Command | Description |
|---|---|
| `init [-c <dir>]` | Generate config directory and key pairs |
| `register -u <user> -i <url>` | Register on an instance |
| `whoami` | Print current profile (no network) |
| `send -t <recipient> [-s <subj>] [file]` | Send a mail |
| `check [inbox\|sent] [id]` | List mailbox or inspect a single mail header |
| `fetch [--raw] <id>` | Decrypt and print a mail; `--raw` outputs body only |
| `burn <id>` | Delete a mail (sender or recipient may burn) |
| `fingerprint [<user>] [--update]` | Compute and compare key fingerprints |
| `rotate` | Rotate key pairs (clears all associated mail) |
| `block <target>` | Block a user or domain |
| `unblock <target>` | Unblock a user or domain |
| `destroy` | Permanently destroy account and local config |
| `man [<command>]` | Display documentation |
| `version` | Print version string |

---

### 11. Worker Configuration (`wrangler.toml`)

| Variable | Default | Description |
|---|---|---|
| `INSTANCE_DOMAIN` | — | Canonical domain of this instance |
| `REGISTRATION_ENABLED` | `true` | Accept new user registrations |
| `RESERVED_USERNAMES` | `admin,system,...` | Comma-separated reserved names |
| `FEDERATION_ENABLED` | `true` | Accept and relay cross-instance mail |
| `MAX_MAIL_SIZE` | `196608` | Max raw payload bytes for `POST /api/mail` |
