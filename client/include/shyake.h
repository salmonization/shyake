#ifndef SHYAKE_H
#define SHYAKE_H

#include <stddef.h>
#include <stdint.h>

/* Opaque pointer for the library context */
typedef struct shyake_ctx shyake_ctx;

/* Configuration options provided by the CLI caller */
typedef struct {
    const char *instance_url;
    const char *config_dir;
    const char *username;
    const char *time_format;
    const char *time_format_recent;
    const char *check_columns;
    int plain;
    int debug;
    int no_color;
} shyake_config;

/* Initialize and free the context */
shyake_ctx* shyake_init_ctx(const shyake_config *config);
void shyake_free_ctx(shyake_ctx *ctx);

/* 
 * Core operations. 
 * Returns 0 on success, non-zero on failure.
 */

/* Generate local ML-KEM and ML-DSA keypairs */
int shyake_generate_keys(shyake_ctx *ctx);

/* 
 * Registration. 
 * username: the desired username.
 * Returns 0 on success, non-zero on failure.
 */
int shyake_register(shyake_ctx *ctx, const char *username);

/* PoW utility */
char* shyake_mint_pow(const char *resource, int bits);

/* 
 * Send a message.
 * recipient: target username.
 * subject: optional subject (max 128 bytes).
 * body: plaintext message body.
 * body_len: length of the body in bytes.
 * Returns 0 on success, non-zero on failure.
 */
int shyake_send(shyake_ctx *ctx, const char *recipient,
                const char *subject, const uint8_t *body,
                size_t body_len);

/* Check options */
typedef struct {
    int count_only;
    int json_out;
    int csv_out;
    int no_header;
} shyake_check_opts;

/* 
 * Check inbox or sent mail.
 * type: "inbox" or "sent".
 * Returns 0 on success, non-zero on failure.
 */
int shyake_check(shyake_ctx *ctx, const char *type,
                 const shyake_check_opts *opts);

/* 
 * Fetch and read a specific mail by ID.
 * mail_id: the 10-char base58 ID.
 * raw: if 1, print only the raw decrypted body.
 * Returns 0 on success, non-zero on failure.
 */
int shyake_fetch(shyake_ctx *ctx, const char *mail_id, int raw);

/*
 * View metadata of a single mail by ID (no body decryption).
 * Returns 0 on success, non-zero on failure.
 */
int shyake_check_one(shyake_ctx *ctx, const char *mail_id);

/*
 * Delete a mail by ID (sender or recipient may burn).
 * Returns 0 on success, non-zero on failure.
 */
int shyake_burn(shyake_ctx *ctx, const char *mail_id);

/*
 * Block or unblock a target (username or instance domain).
 * unblock=0 to block, unblock=1 to unblock.
 * Returns 0 on success, non-zero on failure.
 */
int shyake_block(shyake_ctx *ctx, const char *target, int unblock);

/*
 * Rotate keypairs and upload to the server.
 * Returns 0 on success, non-zero on failure.
 */
int shyake_rotate(shyake_ctx *ctx);

/*
 * Destroy the account and all associated mails locally and remotely.
 * Returns 0 on success, non-zero on failure.
 */
int shyake_destroy(shyake_ctx *ctx);

/*
 * View public key fingerprint of self or a remote user.
 * target_user: NULL for self, or username for remote user.
 * do_update: 1 to overwrite known_hosts with the fetched key, 0 to just view.
 * Returns 0 on success, non-zero on failure.
 */
int shyake_fingerprint(shyake_ctx *ctx, const char *target_user, int do_update);

/* Future E2EE and network operations will be added here */

#endif /* SHYAKE_H */
