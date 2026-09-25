#ifndef SHYAKE_PROMPT_H
#define SHYAKE_PROMPT_H

#include "shyake.h"
#include "internal.h"

/* Read passphrase from stdin with echo suppressed; prompt printed to stderr.
 * Returns 0 on success, -1 on error. buf is NUL-terminated, newline stripped. */
int read_passphrase(const char *prompt, char *buf, usize buflen);

/* Return 1 if the secret key file at path starts with the "SHYK" magic. */
int sk_file_is_encrypted(const char *path);

/* If kem_sk.bin in config_dir is encrypted, prompt for the passphrase and
 * store it in ctx->passphrase. Returns 0 on success, -1 on error. */
int prompt_passphrase(shyake_ctx *ctx, const char *config_dir);

/* Prompt for a new passphrase (with confirmation) and store it in
 * ctx->new_passphrase. Empty input = no passphrase (raw binary).
 * Returns 0 on success, -1 on mismatch or read error. */
int prompt_new_passphrase(shyake_ctx *ctx, const char *config_dir);

#endif /* SHYAKE_PROMPT_H */
