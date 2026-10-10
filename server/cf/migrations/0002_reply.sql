-- Migration: 0002_reply.sql

-- encrypted reference to the mail a reply answers, '' otherwise
ALTER TABLE mail ADD COLUMN enc_reply_to TEXT NOT NULL DEFAULT '';
