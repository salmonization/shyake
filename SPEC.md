# Shyake Protocol Specification

Status: Draft v1 (language-agnostic)

Purpose: Define an end-to-end encrypted, post-quantum mail protocol that any client and any
instance can implement independently.

## Normative Language

The key words `MUST`, `MUST NOT`, `REQUIRED`, `SHOULD`, `SHOULD NOT`, `RECOMMENDED`, `MAY`, and
`OPTIONAL` in this document are to be interpreted as described in RFC 2119.

`Implementation-defined` means the behavior is part of the implementation contract, but this
specification does not prescribe one universal policy. Implementations MUST document the selected
behavior.

`Client` means software that holds a user's secret keys and acts for that user. `Instance` means a
server that hosts accounts and stores and relays mail. `Operator` means the person or organization
that runs an instance.

## 1. Problem Statement

Shyake is a mail protocol. A user writes to another user by address, the way they would with email,
but every instance between them handles only ciphertext. The recipient's client, not any server,
decides whether a mail is authentic.

The protocol solves five problems:

- It keeps the subject, the body, and the reply relation of every mail secret from every instance,
  including the instances of the sender and the recipient.
- It lets the recipient verify who sent a mail and that nobody changed it, without trusting any
  instance.
- It uses post-quantum algorithms for every public-key operation, so ciphertext collected today
  stays protected against a future quantum adversary.
- It lets anyone run an instance at almost no cost, as a serverless worker or a single binary. Open
  public instances and private self-hosted instances exchange mail with each other.
- It limits the damage of a stolen key: a user's encryption keys rotate on their own, and old secret
  keys are deleted, so a key stolen today does not open mail from earlier periods.

Important boundary:

- This document defines a protocol, not a client. It specifies what every client and every
  instance MUST do to interoperate and to stay secure.
- Commands, user interface, configuration, and local storage layout are not part of the protocol.
  Each client defines its own.
- A conforming client is conforming because it follows this document, not because it uses a
  particular library. `libshyake` is one implementation of the client-side protocol, not its
  definition.

## 2. Goals and Non-Goals

### 2.1 Goals

- Encrypt the subject, the body, and the reply reference of every mail end to end.
- Let the recipient's client verify the sender's signature on every mail before it shows any
  content.
- Use post-quantum algorithms (ML-KEM-768, ML-DSA-65) for all key encapsulation and signatures.
- Provide forward secrecy per key period: encryption keys rotate automatically, and retired secret
  keys are deleted.
- Make key rotation invisible to users: a user verifies a contact's identity once, out of band, and
  never again for routine rotation.
- Give every mail one content-addressed identifier that is the same on every instance and every
  client.
- Let open public instances and self-hosted instances federate on equal terms.
- Keep the protocol small enough that an implementer can build a client or an instance in any
  language from this document and the conformance vectors alone.
- Let a user move an account between clients and between instances.

### 2.2 Non-Goals

- Hiding from instances who sends mail to whom. Instances see addresses and timestamps (Section
  3.4). Users who need more metadata privacy run their own instance.
- Key escrow, or any way for an operator or administrator to read mail. The protocol has no such
  mechanism and MUST NOT gain one.
- Per-message forward secrecy or ratcheting. The protocol does not assume that both parties are
  online. Mail can wait unread for weeks.
- Multi-device accounts. In this version an account is active on one device at a time (Appendix A).
- Guaranteed deletion of every copy. Burn removes the copies on instances but cannot remove a copy
  that a client has saved.
- Store-and-forward queues. Instances relay mail synchronously. A client that fails to send keeps
  the mail and retries.
- Interoperability with SMTP, IMAP, or OpenPGP.
- Binary attachments. A mail body is UTF-8 text in this version.
- Prescribing commands, a user interface, or local storage layout.

## 3. System Overview

### 3.1 Roles

1. `Client`
   - Generates and holds the user's secret keys. Secret keys never leave the client, except inside
     an encrypted account export (Section 14).
   - Encrypts, signs, verifies, and decrypts mail.
   - Pins the identity keys of contacts and tracks their verification state.
   - Rotates the user's encryption keys and deletes retired secret keys.

2. `Instance`
   - Hosts accounts: stores each account's identity public key and current key endorsement.
   - Accepts mail from its own accounts, delivers it locally, or relays it to the recipient's
     instance.
   - Accepts mail relayed from other instances for its own accounts.
   - Stores envelopes and serves them to the account that owns the mailbox.
   - Enforces its instance policy (Section 12) and abuse limits (Section 13).
   - Never holds a secret key of any user and never sees plaintext.

3. `Operator`
   - Deploys and configures an instance.
   - Chooses the instance policy: open registration, invitation only, or closed; open federation,
     an allowlist, or none.
   - Has no access to mail content. The operator sees what the instance sees (Section 3.4).

### 3.2 Abstraction Layers

Shyake is easiest to implement when kept in these layers:

1. `Primitive Layer`
   - ML-KEM-768, ML-DSA-65, ChaCha20-Poly1305, SHA-256, HKDF-SHA-256, scrypt.
   - Domain separation labels (Section 5).

2. `Object Layer`
   - The binary encoding of key endorsements, envelopes, sealed content, and account exports
     (Section 6, Section 14).
   - Pure functions: bytes in, bytes or a verified object out. No I/O.

3. `Protocol Layer`
   - The HTTP API, request authentication, delivery, federation, and burn (Sections 9 to 11).

4. `Client Behavior Layer`
   - Trust decisions, key rotation, retirement, and what a client MUST check before it shows
     content (Sections 7, 8, 15).

5. `Implementation Layer` (not specified)
   - Commands, user interface, configuration, local storage, packaging, and updates.

Design note: the object layer has no I/O so that it can be tested against the conformance vectors in
isolation and shared between clients and instances written in the same language.

### 3.3 Threat Model

The protocol assumes that any instance, including the user's own, can be malicious or compromised.

An instance can:

- Read the metadata listed in Section 3.4.
- Refuse, drop, or delay mail.
- Refuse to burn a mail, or keep a copy after it reports a burn.
- Serve a wrong identity key for a user that the client has not contacted before. Out-of-band
  verification (Section 8) detects this.
- Replay an old envelope to a client. The client detects a repeated mail by its mail ID.

An instance cannot, against a conforming client:

- Read the subject, the body, or the reply reference of a mail.
- Forge a mail from any user, or change a mail without detection.
- Substitute the identity key of a contact that the client has already pinned without the client
  raising an alert.
- Make a client encrypt to an encryption key that the recipient's identity key has not endorsed.

The protocol does not protect against:

- A compromised client device. An attacker with the device and the passphrase has everything the
  user has.
- Traffic analysis by a network observer beyond what TLS hides.

### 3.4 Metadata Exposure

Each instance that stores or relays a mail sees:

- The sender address and the recipient address.
- The time it received the mail, and the creation time the sender's client put in the envelope.
- The padded size of the mail (Section 6), not its exact size.
- The mail ID, and therefore whether two instances hold the same mail.
- The time an account fetches or burns a mail.
- The client's network address, unless the client hides it.

An instance does not see:

- The subject, the body, or the reply reference. The instance cannot tell whether a mail is a reply
  or which mail it answers.

On a public instance the operator sees this metadata for every account on that instance. When
sender and recipient share an instance, one operator sees both sides of the conversation.

### 3.5 External Dependencies

- ML-KEM-768: FIPS 203.
- ML-DSA-65: FIPS 204, with the context string parameter.
- ChaCha20-Poly1305: RFC 8439.
- SHA-256: FIPS 180-4.
- HKDF-SHA-256: RFC 5869.
- scrypt: RFC 7914.
- Transport: HTTPS (TLS 1.2 or later). An instance MAY serve plain HTTP only on a loopback address
  for development.

## 4. Core Domain Model

### 4.1 Entities

#### 4.1.1 Address

Names an account on an instance.

Fields:

- `local` (string)
  - REQUIRED. 4 to 32 characters from `a-z`, `0-9`, and `_`, with at least one letter.
  - Case-insensitive. The canonical form is lowercase.
- `host` (string)
  - REQUIRED. The host name of the instance, with an optional port (example:
    `mail.example.org`, `shyake-salmon.workers.dev`, `127.0.0.1:8787`).
  - Case-insensitive. The canonical form is lowercase. The port is omitted when it is 443.

The canonical text form is `local@host`. The protocol always carries the full canonical form. A
client MAY let users type only `local` for an account on their own instance, but it MUST expand it
before it builds an envelope or a request.

Important boundary: the host in an address is the instance that serves it. There is no delegation.
`coffee@example.org` is served by `https://example.org`.

#### 4.1.2 Identity

A user's long-term signing key. The identity is what other users verify and pin.

Fields:

- `identity_key` (bytes)
  - REQUIRED. ML-DSA-65 public key.
- `fingerprint` (bytes)
  - Derived: SHA-256 over a domain-separated encoding of `identity_key` (Section 8).

An identity does not contain an address. The same identity can move to another address (Section
8). An identity does not change during normal use. A new identity key is a new identity.

#### 4.1.3 Key Endorsement

An encryption public key, signed by an identity. Senders encrypt only to endorsed keys.

Fields:

- `kem_key` (bytes)
  - REQUIRED. ML-KEM-768 public key.
- `kem_key_id` (bytes, 8)
  - Derived: the first 8 bytes of SHA-256 over `kem_key`.
- `not_before` (integer, Unix seconds)
  - REQUIRED. When the key becomes valid.
- `not_after` (integer, Unix seconds)
  - REQUIRED. After this time senders MUST NOT encrypt to the key.
- `signature` (bytes)
  - REQUIRED. ML-DSA-65 signature by the identity key over the encoded fields above.

#### 4.1.4 Envelope

The signed, encrypted unit of mail. Instances store and relay envelopes byte for byte.

Fields:

- `version` (integer)
  - REQUIRED. `1` for this specification.
- `sender` (Address)
- `recipient` (Address)
- `created_at` (integer, Unix seconds)
  - Set by the sender's client.
- `key_slots` (list of Key Slot)
  - REQUIRED. At least one slot for the recipient. A conforming client also adds one slot for the
    sender, so that the sender can read their own sent mail.
- `sealed_content` (bytes)
  - REQUIRED. Sealed Content (Section 4.1.6), encrypted with the content key.
- `signature` (bytes)
  - REQUIRED. ML-DSA-65 signature by the sender's identity key over all preceding fields.

The binary layout is in Section 6.

#### 4.1.5 Key Slot

One copy of the content key, encapsulated to one encryption key.

Fields:

- `kem_key_id` (bytes, 8)
  - Names the endorsed key the slot is encapsulated to.
- `kem_ciphertext` (bytes)
  - ML-KEM-768 ciphertext.
- `wrapped_key` (bytes)
  - The content key, encrypted with a key derived from the KEM shared secret (Section 5).

Design note: the envelope carries a list of slots, not exactly two, so that a future multi-device
extension (Appendix A) does not change the envelope format.

#### 4.1.6 Sealed Content

The plaintext a client encrypts as one unit. Instances never see it.

Fields:

- `subject` (string, UTF-8)
  - At most 256 bytes. MAY be empty.
- `body` (string, UTF-8)
  - MAY be empty.
- `in_reply_to` (Mail ID or none)
  - The mail this mail answers. None for a mail that starts a thread.
- `padding` (bytes)
  - Zero bytes that round the encoded size up to a size bucket (Section 6).

Important boundary: subject, body, and reply reference are encrypted together, under one key and
one nonce. An instance cannot swap one field for another.

#### 4.1.7 Mail ID

The identifier of a mail.

- The lowercase hex encoding of SHA-256 over the complete encoded envelope, signature included.
- The same on every instance and every client that holds the mail.
- A client MUST accept the full Mail ID. It MAY also accept a prefix, resolved against the mail the
  client knows, and MUST reject a prefix that matches more than one known mail.
- A client that shows a shortened Mail ID MUST show at least 7 hex digits, and more when 7 digits
  do not tell the known mail apart.
- The HTTP API (Section 9) accepts only full Mail IDs.

#### 4.1.8 Mailbox Entry

An instance's record of one envelope for one account.

Fields:

- `mail_id` (Mail ID)
- `envelope` (bytes)
- `folder` (enum)
  - `inbox` when the account is the recipient and `sent` when the account is the sender.
    A mail from an account to itself appears in both.
- `received_at` (integer, Unix seconds)
  - Set by the instance.

#### 4.1.9 Contact

A client's record of another user's identity.

Fields:

- `address` (Address)
- `identity_key` (bytes)
- `state` (enum)
  - `tofu`: pinned on first contact, not verified out of band.
  - `verified`: the user compared the fingerprint through a trusted channel.
- `first_seen` (integer, Unix seconds)

Contacts are client state. A client MUST NOT send contact records to an instance. An instance can
infer from metadata whom an account mails (Section 3.4), but it MUST NOT learn which contacts the
user has verified: that would tell an attacker which identity keys it could substitute without
detection. Contacts are part of an account export (Section 14).

#### 4.1.10 Instance Descriptor

What an instance publishes about itself (Section 12).

Fields:

- `protocol` (integer)
  - REQUIRED. The protocol version. `1` for this specification.
- `registration` (enum)
  - REQUIRED. `open`, `invite`, or `closed`.
- `federation` (enum)
  - REQUIRED. `open`, `allowlist`, or `off`.
- `max_envelope_size` (integer, bytes)
  - REQUIRED.
- `pow_bits` (integer)
  - REQUIRED. The proof-of-work difficulty for requests that need one (Section 13).
- `implementation` (string)
  - OPTIONAL. Free text naming the server software and version.

### 4.2 Identifiers and Normalization Rules

- Addresses are compared in canonical form (lowercase, default port omitted). An instance MUST
  canonicalize an address before it stores, compares, or relays it.
- Two accounts on one instance MUST NOT have the same canonical address.
- Mail IDs, fingerprints, and key IDs are compared as bytes. Their text form is lowercase hex.
- All integers on the wire are unsigned and big-endian unless a section says otherwise.
- All times are Unix seconds in UTC.
