#ifndef SHYAKE_LIB_INTERNAL_H
#define SHYAKE_LIB_INTERNAL_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <curl/curl.h>
#include "shyake.h"
#include "internal.h"

/* Internal context definition hidden from the public ABI */
struct shyake_ctx {
	char *instance_url;
	char *config_dir;
	char *username;
	int plain;
	int debug;
	int no_color;
	char passphrase[512]; /* current passphrase for loading encrypted sk */
	char new_passphrase[512]; /* new passphrase for saving sk (rotate only) */
	char last_error[512]; /* detail of the last failure, "" if none */
};

/* libshyake.c (error reporting) */
void set_error(shyake_ctx *ctx, const char *fmt, ...)
	__attribute__((format(printf, 2, 3)));

/* libcurl response buffer */
struct curl_response {
	char *data;
	usize size;
};

/* network.c */
usize curl_write_cb(void *contents, usize size, usize nmemb, void *userp);
struct curl_slist *create_signed_headers(shyake_ctx *ctx, const char *method,
					 const char *endpoint,
					 const char *username);
struct curl_slist *create_auth_headers(shyake_ctx *ctx, const char *endpoint,
				       const char *username);
char *fetch_recipient_pubkey(shyake_ctx *ctx, const char *recipient);

/* libshyake.c (file I/O & base64) */
int save_file(const char *path, const u8 *data, usize len);
u8 *load_file(const char *path, usize *len);

/* passphrase.c */
void zero_memory(void *buf, usize len);
int save_sk_encrypted(const char *path, const char *passphrase, const u8 *sk,
		      usize sk_len);
u8 *load_sk_decrypted(shyake_ctx *ctx, const char *path, usize *out_len);
char *base64_encode(const u8 *data, usize len);
u8 *base64_decode(const char *b64, usize *out_len);

/* crypto_ops.c */
char *encrypt_to_b64(const u8 *key, const u8 *pt, usize pt_len);
char *decrypt_from_b64(const u8 *key, const char *b64);
char *kem_encapsulate_key(const u8 *kem_pk, usize kem_pk_len,
			  const u8 *sym_key);
u8 *kem_decapsulate_key(const char *enc_key_b64, const u8 *ksk);

/* known_hosts.c */
char *get_known_host(const char *config_dir, const char *username);
void add_known_host(const char *config_dir, const char *username,
		    const char *fp, const char *pk_b64);
void update_known_host(const char *config_dir, const char *username,
		       const char *fp, const char *pk_b64);

#endif /* SHYAKE_LIB_INTERNAL_H */
