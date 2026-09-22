-- Same tables and columns as server/cf/migrations/0001_initial.sql,
-- plus what the Go server needs on top: case-insensitive name
-- uniqueness, a signature hash for idempotent submission, and the
-- federation relay queue.

CREATE TABLE users (
    username   TEXT PRIMARY KEY,
    kem_pubkey TEXT NOT NULL,
    sig_pubkey TEXT NOT NULL,
    created_at INTEGER NOT NULL
);

-- "Alice" cannot sit next to "alice" (SPEC §4); lookups stay exact
CREATE UNIQUE INDEX users_username_nocase ON users (username COLLATE NOCASE);

CREATE TABLE mail (
    mail_id           TEXT PRIMARY KEY,
    sender            TEXT NOT NULL,
    recipient         TEXT NOT NULL,
    enc_key_sender    TEXT NOT NULL,
    enc_key_recipient TEXT NOT NULL,
    enc_subject       TEXT NOT NULL,
    enc_body          TEXT NOT NULL,
    size              INTEGER NOT NULL,
    signature         TEXT NOT NULL,
    timestamp         INTEGER NOT NULL,
    -- sha256 of signature: a resubmitted or re-relayed mail is
    -- recognized instead of stored twice
    sig_hash          BLOB NOT NULL UNIQUE
);

CREATE INDEX mail_recipient ON mail (recipient, timestamp DESC);
CREATE INDEX mail_sender ON mail (sender, timestamp DESC);

CREATE TABLE blocks (
    blocker    TEXT NOT NULL,
    blocked    TEXT NOT NULL,
    created_at INTEGER NOT NULL,
    PRIMARY KEY (blocker, blocked)
);

CREATE INDEX blocks_blocked ON blocks (blocked);

CREATE TABLE relay_outbox (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    mail_id     TEXT NOT NULL,
    domain      TEXT NOT NULL,
    payload     TEXT NOT NULL,
    signed_at   INTEGER NOT NULL,
    attempts    INTEGER NOT NULL DEFAULT 0,
    next_try_at INTEGER NOT NULL,
    status      TEXT NOT NULL DEFAULT 'pending', -- pending | dead
    last_error  TEXT NOT NULL DEFAULT '',
    created_at  INTEGER NOT NULL
);

CREATE INDEX relay_outbox_due ON relay_outbox (status, next_try_at);
