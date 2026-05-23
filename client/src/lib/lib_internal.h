#ifndef SHYAKE_LIB_INTERNAL_H
#define SHYAKE_LIB_INTERNAL_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <curl/curl.h>
#include "shyake.h"

/* Internal context definition hidden from the public ABI */
struct shyake_ctx {
    char *instance_url;
    char *config_dir;
    char *username;
    char *time_format;
    char *time_format_recent;
    char *check_columns;
    int plain;
    int debug;
    int no_color;
};

/* libcurl response buffer */
struct curl_response {
    char *data;
    size_t size;
};

/* network.c */
size_t curl_write_cb(void *contents, size_t size, size_t nmemb, void *userp);
struct curl_slist* create_signed_headers(shyake_ctx *ctx,
                                         const char *method,
                                         const char *endpoint,
                                         const char *username);
struct curl_slist* create_auth_headers(shyake_ctx *ctx,
                                       const char *endpoint,
                                       const char *username);
char* fetch_recipient_pubkey(shyake_ctx *ctx, const char *recipient);

/* libshyake.c (file I/O & base64) */
int save_file(const char *path, const uint8_t *data, size_t len);
uint8_t* load_file(const char *path, size_t *len);
char* base64_encode(const uint8_t *data, size_t len);
uint8_t* base64_decode(const char *b64, size_t *out_len);

/* crypto_ops.c */
char* encrypt_to_b64(const uint8_t *key, const uint8_t *pt, size_t pt_len);
char* decrypt_from_b64(const uint8_t *key, const char *b64);
char* kem_encapsulate_key(const uint8_t *kem_pk, size_t kem_pk_len,
                          const uint8_t *sym_key);
uint8_t* kem_decapsulate_key(const char *enc_key_b64, const uint8_t *ksk);

/* known_hosts.c */
char* get_known_host(const char *config_dir, const char *username);
void add_known_host(const char *config_dir, const char *username,
                    const char *fp, const char *pk_b64);
void update_known_host(const char *config_dir, const char *username,
                       const char *fp, const char *pk_b64);

#endif /* SHYAKE_LIB_INTERNAL_H */
