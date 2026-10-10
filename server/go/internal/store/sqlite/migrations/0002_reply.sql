-- Replies (SPEC §3.2): the encrypted reference to the mail answered,
-- '' for any other mail. Same column as server/cf/migrations.
ALTER TABLE mail ADD COLUMN enc_reply_to TEXT NOT NULL DEFAULT '';
