-- Migration: 0001_initial.sql

-- users table
CREATE TABLE IF NOT EXISTS users (
    username TEXT PRIMARY KEY,
    kem_pubkey TEXT NOT NULL,
    sig_pubkey TEXT NOT NULL,
    created_at INTEGER NOT NULL
);

-- mail table
CREATE TABLE IF NOT EXISTS mail (
    mail_id TEXT PRIMARY KEY,
    sender TEXT NOT NULL,
    recipient TEXT NOT NULL,
    enc_key_sender TEXT NOT NULL,
    enc_key_recipient TEXT NOT NULL,
    enc_subject TEXT NOT NULL,
    enc_body TEXT NOT NULL,
    size INTEGER NOT NULL,
    signature TEXT NOT NULL,
    timestamp INTEGER NOT NULL
);

CREATE INDEX IF NOT EXISTS idx_mail_recipient ON mail(recipient);
CREATE INDEX IF NOT EXISTS idx_mail_sender ON mail(sender);

-- blocks table
CREATE TABLE IF NOT EXISTS blocks (
    blocker TEXT NOT NULL,
    blocked TEXT NOT NULL,
    created_at INTEGER NOT NULL,
    PRIMARY KEY (blocker, blocked)
);
