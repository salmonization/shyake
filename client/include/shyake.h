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
    int plain;
    int debug;
    int no_color;
} shyake_config;

/* Initialize and free the context */
shyake_ctx* shyake_init_ctx(const shyake_config *config);
void shyake_free_ctx(shyake_ctx *ctx);

/* Generate local ML-KEM and ML-DSA keypairs */
int shyake_generate_keys(shyake_ctx *ctx);

/* PoW utility */
char* shyake_mint_pow(const char *resource, int bits);

/* Registration */
int shyake_register(shyake_ctx *ctx, const char *username);

/*
 * Send a message.
 * Returns 0 on success, non-zero on failure.
 */
int shyake_send(shyake_ctx *ctx, const char *recipient,
                const char *subject, const uint8_t *body,
                size_t body_len);

/* --- Mail list --- */

typedef struct {
    char *mail_id;
    char *party;       /* inbox: sender; sent: recipient (raw string) */
    char *subject;     /* decrypted, NULL if failed */
    int size;          /* plaintext byte count */
    int64_t timestamp; /* UNIX timestamp seconds */
    int is_sent;       /* 1 if from sent box */
} shyake_mail_entry;

typedef struct {
    shyake_mail_entry *entries;
    int count;
} shyake_mail_list;

void shyake_free_mail_list(shyake_mail_list *list);

/*
 * Fetch inbox or sent mail list.
 * type: "inbox" or "sent".
 * Returns allocated shyake_mail_list* on success, NULL on failure.
 * Caller must free with shyake_free_mail_list().
 */
shyake_mail_list* shyake_check(shyake_ctx *ctx, const char *type);

/* --- Mail detail --- */

typedef struct {
    char *mail_id;
    char *sender;
    char *recipient;
    char *subject;     /* decrypted, NULL if failed */
    char *body;        /* decrypted, NULL if failed */
    int64_t timestamp; /* UNIX timestamp seconds */
    int size;          /* plaintext byte count */
} shyake_mail_detail;

void shyake_free_mail_detail(shyake_mail_detail *d);

/*
 * Fetch full content of a single mail (decrypt body).
 * Returns allocated shyake_mail_detail* on success, NULL on failure.
 */
shyake_mail_detail* shyake_fetch(shyake_ctx *ctx, const char *mail_id);

/*
 * Fetch metadata of a single mail (no body decryption).
 * Returns allocated shyake_mail_detail* on success, NULL on failure.
 */
shyake_mail_detail* shyake_check_one(shyake_ctx *ctx, const char *mail_id);

/*
 * Delete a mail by ID.
 * Returns 0 on success, non-zero on failure.
 */
int shyake_burn(shyake_ctx *ctx, const char *mail_id);

/*
 * Block or unblock a target.
 * unblock=0 to block, unblock=1 to unblock.
 */
int shyake_block(shyake_ctx *ctx, const char *target, int unblock);

/* Rotate keypairs and upload to server */
int shyake_rotate(shyake_ctx *ctx);

/* Destroy account and all associated data */
int shyake_destroy(shyake_ctx *ctx);

/* --- Fingerprint --- */

typedef struct {
    unsigned char local_fp[32];  /* SHA-256 of known_hosts key */
    unsigned char remote_fp[32]; /* SHA-256 of server key */
    int has_local;  /* 1 if a known_hosts entry exists */
    int match;      /* 1=MATCH, 0=MISMATCH, relevant only if has_local */
} shyake_fp_result;

void shyake_free_fp_result(shyake_fp_result *r);

/*
 * Compute fingerprint of self or a remote user.
 * target_user: NULL for self, username for remote.
 * do_update: 1 to rewrite known_hosts with fetched key.
 * Returns allocated shyake_fp_result* on success, NULL on failure.
 * Self-fingerprint: only remote_fp is populated (from local kem_pk.bin).
 */
shyake_fp_result* shyake_fingerprint(shyake_ctx *ctx,
                                     const char *target_user,
                                     int do_update);

#endif /* SHYAKE_H */
