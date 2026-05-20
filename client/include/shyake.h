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

/* 
 * Check inbox or sent mail.
 * type: "inbox" or "sent".
 * Returns 0 on success, non-zero on failure.
 */
int shyake_check(shyake_ctx *ctx, const char *type);

/* 
 * Fetch and read a specific mail by ID.
 * mail_id: the 10-char base58 ID.
 * raw: if 1, print only the raw decrypted body.
 * Returns 0 on success, non-zero on failure.
 */
int shyake_fetch(shyake_ctx *ctx, const char *mail_id, int raw);

/* Future E2EE and network operations will be added here */

#endif /* SHYAKE_H */
