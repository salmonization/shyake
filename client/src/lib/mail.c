#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <oqs/oqs.h>
#include <openssl/sha.h>
#include <curl/curl.h>
#include "vendor/cJSON/cJSON.h"
#include "lib_internal.h"

/* ------------------------------------------------------------------ */

void
shyake_free_mail_list(shyake_mail_list *list)
{
    if (!list) return;
    for (int i = 0; i < list->count; i++) {
        free(list->entries[i].mail_id);
        free(list->entries[i].party);
        free(list->entries[i].subject);
    }
    free(list->entries);
    free(list);
}

void
shyake_free_mail_detail(shyake_mail_detail *d)
{
    if (!d) return;
    free(d->mail_id);
    free(d->sender);
    free(d->recipient);
    free(d->subject);
    free(d->body);
    free(d);
}

/* ------------------------------------------------------------------ */

shyake_err
shyake_send(shyake_ctx *ctx, const char *recipient,
            const char *subject, const uint8_t *body,
            size_t body_len)
{
    if (!ctx || !recipient || !body) return SHYAKE_ERR;

    /* Fetch recipient public key */
    char *recip_pk_b64 = fetch_recipient_pubkey(ctx, recipient);
    if (!recip_pk_b64)
        return SHYAKE_ERR_NETWORK;

    size_t recip_pk_len;
    uint8_t *recip_pk = base64_decode(recip_pk_b64, &recip_pk_len);
    if (!recip_pk) {
        free(recip_pk_b64);
        return SHYAKE_ERR_CRYPTO;
    }

    /* Compute recipient fingerprint */
    unsigned char fp_raw[SHA256_DIGEST_LENGTH];
    SHA256(recip_pk, recip_pk_len, fp_raw);
    char fp_hex[SHA256_DIGEST_LENGTH * 2 + 1];
    for (int i = 0; i < SHA256_DIGEST_LENGTH; i++)
        sprintf(fp_hex + (i * 2), "%02x", fp_raw[i]);

    /* Check known_hosts */
    char *known_pk_b64 = get_known_host(ctx->config_dir, recipient);
    if (known_pk_b64) {
        size_t known_pk_len;
        uint8_t *known_pk = base64_decode(known_pk_b64, &known_pk_len);
        free(known_pk_b64);
        if (known_pk) {
            unsigned char known_fp[SHA256_DIGEST_LENGTH];
            SHA256(known_pk, known_pk_len, known_fp);
            free(known_pk);
            if (memcmp(known_fp, fp_raw, SHA256_DIGEST_LENGTH) != 0) {
                free(recip_pk_b64); free(recip_pk);
                return SHYAKE_ERR_KEY_MISMATCH;
            }
        }
    } else {
        add_known_host(ctx->config_dir, recipient, fp_hex, recip_pk_b64);
    }

    /* Load own keys */
    size_t my_kpk_len, my_ssk_len;
    char path[512];
    snprintf(path, sizeof(path), "%s/kem_pk.bin", ctx->config_dir);
    uint8_t *my_kpk = load_file(path, &my_kpk_len);
    snprintf(path, sizeof(path), "%s/sig_sk.bin", ctx->config_dir);
    uint8_t *my_ssk = load_file(path, &my_ssk_len);

    if (!my_kpk || !my_ssk) {
        free(recip_pk_b64); free(recip_pk);
        free(my_kpk); free(my_ssk);
        return SHYAKE_ERR_CRYPTO;
    }

    /* Generate symmetric key */
    uint8_t sym_key[32];
    size_t read_bytes = 0;
    FILE *urandom = fopen("/dev/urandom", "rb");
    if (urandom) {
        read_bytes = fread(sym_key, 1, 32, urandom);
        fclose(urandom);
    }
    if (read_bytes != 32) {
        free(recip_pk_b64); free(recip_pk); free(my_kpk); free(my_ssk);
        return SHYAKE_ERR_CRYPTO;
    }

    char *enc_subject = encrypt_to_b64(
        sym_key, (const uint8_t*)subject,
        subject ? strlen(subject) : 0);
    char *enc_body = encrypt_to_b64(sym_key, body, body_len);
    char *enc_key_recipient = kem_encapsulate_key(
        recip_pk, recip_pk_len, sym_key);
    char *enc_key_sender = kem_encapsulate_key(
        my_kpk, my_kpk_len, sym_key);

    if (!enc_key_recipient || !enc_key_sender) {
        free(recip_pk_b64); free(recip_pk); free(my_kpk); free(my_ssk);
        free(enc_subject); free(enc_body);
        free(enc_key_recipient); free(enc_key_sender);
        return SHYAKE_ERR_CRYPTO;
    }

    /* Resolve sender string */
    char sender_buf[256];
    const char *sender = ctx->username;
    if (strchr(recipient, '@') != NULL) {
        const char *d = strstr(ctx->instance_url, "://");
        d = d ? d + 3 : ctx->instance_url;
        char domain[128] = {0};
        const char *sl = strchr(d, '/');
        if (sl) {
            size_t ln = (size_t)(sl - d);
            if (ln >= sizeof(domain)) ln = sizeof(domain) - 1;
            strncpy(domain, d, ln);
        } else {
            strncpy(domain, d, sizeof(domain) - 1);
        }
        snprintf(sender_buf, sizeof(sender_buf), "%s@%s",
                 ctx->username, domain);
        sender = sender_buf;
    }

    time_t now = time(NULL);
    char timestamp[32];
    snprintf(timestamp, sizeof(timestamp), "%ld", now);

    cJSON *signed_obj = cJSON_CreateObject();
    cJSON_AddStringToObject(signed_obj, "sender", sender);
    cJSON_AddStringToObject(signed_obj, "recipient", recipient);
    cJSON_AddStringToObject(signed_obj, "recipient_kem_fingerprint", fp_hex);
    cJSON_AddStringToObject(signed_obj, "enc_subject",
                            enc_subject ? enc_subject : "");
    cJSON_AddStringToObject(signed_obj, "enc_body", enc_body);
    cJSON_AddStringToObject(signed_obj, "timestamp", timestamp);
    cJSON_AddNumberToObject(signed_obj, "size", body_len);

    char *json_raw = cJSON_PrintUnformatted(signed_obj);

    OQS_SIG *sig = OQS_SIG_new("ML-DSA-65");
    uint8_t *signature = malloc(sig->length_signature);
    size_t sig_len;
    OQS_SIG_sign(sig, signature, &sig_len, (uint8_t*)json_raw,
                 strlen(json_raw), my_ssk);
    char *sig_b64 = base64_encode(signature, sig_len);

    char *pow = shyake_mint_pow(sender, 20);

    cJSON *root = cJSON_Duplicate(signed_obj, 1);
    cJSON_AddStringToObject(root, "enc_key_sender", enc_key_sender);
    cJSON_AddStringToObject(root, "enc_key_recipient", enc_key_recipient);
    cJSON_AddStringToObject(root, "signature", sig_b64);
    cJSON_AddStringToObject(root, "pow", pow);
    char *payload = cJSON_PrintUnformatted(root);

    shyake_err ret = SHYAKE_OK;
    CURL *curl = curl_easy_init();
    if (curl) {
        if (ctx->debug)
            curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L);

        char url[512];
        snprintf(url, sizeof(url), "%s/api/mail", ctx->instance_url);

        struct curl_slist *headers = NULL;
        headers = curl_slist_append(headers,
                                    "Content-Type: application/json");

        struct curl_response resp = { .data = malloc(1), .size = 0 };
        resp.data[0] = '\0';

        curl_easy_setopt(curl, CURLOPT_URL, url);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload);
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&resp);

        CURLcode res = curl_easy_perform(curl);
        if (res != CURLE_OK) {
            ret = SHYAKE_ERR_NETWORK;
        } else {
            long http_code = 0;
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
            if (http_code == 200 || http_code == 201) {
                ret = SHYAKE_OK;
            } else if (http_code == 409) {
                ret = SHYAKE_ERR_KEY_MISMATCH;
            } else if (http_code == 410) {
                ret = SHYAKE_ERR_GONE;
            } else {
                ret = SHYAKE_ERR_HTTP;
            }
        }

        free(resp.data);
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
    } else {
        ret = SHYAKE_ERR;
    }

    free(recip_pk_b64); free(recip_pk); free(my_kpk); free(my_ssk);
    free(enc_subject); free(enc_body);
    free(enc_key_recipient); free(enc_key_sender);
    free(json_raw); free(payload); free(signature);
    free(sig_b64); free(pow);
    cJSON_Delete(root); cJSON_Delete(signed_obj);
    OQS_SIG_free(sig);
    return ret;
}

/* ------------------------------------------------------------------ */

shyake_mail_list*
shyake_check(shyake_ctx *ctx, const char *type)
{
    if (!ctx || !type) return NULL;
    const char *username = ctx->username;

    char endpoint[128];
    snprintf(endpoint, sizeof(endpoint), "/api/mail?type=%s", type);
    char url[512];
    snprintf(url, sizeof(url), "%s%s", ctx->instance_url, endpoint);

    CURL *curl = curl_easy_init();
    if (!curl) return NULL;

    if (ctx->debug)
        curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L);

    struct curl_slist *headers = create_auth_headers(
        ctx, endpoint, username);
    if (!headers) { curl_easy_cleanup(curl); return NULL; }

    struct curl_response resp = { .data = malloc(1), .size = 0 };
    resp.data[0] = '\0';

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&resp);

    CURLcode res = curl_easy_perform(curl);
    shyake_mail_list *result = NULL;

    if (res == CURLE_OK) {
        long http_code;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
        if (http_code == 200) {
            cJSON *json = cJSON_Parse(resp.data);
            if (json) {
                cJSON *mail_arr = cJSON_GetObjectItem(json, "mail");
                if (cJSON_IsArray(mail_arr)) {
                    int count = cJSON_GetArraySize(mail_arr);
                    result = calloc(1, sizeof(shyake_mail_list));
                    result->count = count;
                    result->entries = calloc(
                        count, sizeof(shyake_mail_entry));

                    int is_sent = (strcmp(type, "sent") == 0);

                    /* load KEM secret key for decryption */
                    char path[512];
                    size_t ksk_len;
                    snprintf(path, sizeof(path), "%s/kem_sk.bin",
                             ctx->config_dir);
                    uint8_t *ksk = load_file(path, &ksk_len);

                    for (int i = 0; i < count; i++) {
                        cJSON *item = cJSON_GetArrayItem(mail_arr, i);
                        const char *id = cJSON_GetObjectItem(
                            item, "mail_id")->valuestring;
                        const char *party = is_sent
                            ? cJSON_GetObjectItem(
                                item, "recipient")->valuestring
                            : cJSON_GetObjectItem(
                                item, "sender")->valuestring;
                        int sz = cJSON_GetObjectItem(
                            item, "size")->valueint;
                        int ts = cJSON_GetObjectItem(
                            item, "timestamp")->valueint;

                        const char *ekf = is_sent
                            ? "enc_key_sender" : "enc_key_recipient";
                        const char *enc_key = cJSON_GetObjectItem(
                            item, ekf)->valuestring;
                        const char *enc_sub = cJSON_GetObjectItem(
                            item, "enc_subject")->valuestring;

                        char *sub = NULL;
                        if (ksk) {
                            uint8_t *sym = kem_decapsulate_key(
                                enc_key, ksk);
                            if (sym) {
                                sub = decrypt_from_b64(sym, enc_sub);
                                free(sym);
                            }
                        }

                        shyake_mail_entry *e = &result->entries[i];
                        e->mail_id   = strdup(id);
                        e->party     = strdup(party);
                        e->subject   = sub ? sub
                                         : strdup("(decryption failed)");
                        e->size      = sz;
                        e->timestamp = (int64_t)ts;
                        e->is_sent   = is_sent;
                    }
                    free(ksk);
                }
                cJSON_Delete(json);
            }
        } else {
            fprintf(stderr, "Failed to check mail (HTTP %ld): %s\n",
                    http_code, resp.data);
        }
    } else {
        fprintf(stderr, "Network error: %s\n",
                curl_easy_strerror(res));
    }

    free(resp.data);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return result;
}

/* ------------------------------------------------------------------ */

shyake_mail_detail*
shyake_fetch(shyake_ctx *ctx, const char *mail_id)
{
    if (!ctx || !mail_id) return NULL;
    const char *username = ctx->username;

    char endpoint[128];
    snprintf(endpoint, sizeof(endpoint), "/api/mail/%s", mail_id);
    char url[512];
    snprintf(url, sizeof(url), "%s%s", ctx->instance_url, endpoint);

    CURL *curl = curl_easy_init();
    if (!curl) return NULL;

    if (ctx->debug)
        curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L);

    struct curl_slist *headers = create_auth_headers(
        ctx, endpoint, username);
    if (!headers) { curl_easy_cleanup(curl); return NULL; }

    struct curl_response resp = { .data = malloc(1), .size = 0 };
    resp.data[0] = '\0';

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&resp);

    CURLcode res = curl_easy_perform(curl);
    shyake_mail_detail *result = NULL;

    if (res == CURLE_OK) {
        long http_code;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
        if (http_code == 200) {
            cJSON *json = cJSON_Parse(resp.data);
            if (json) {
                const char *snd = cJSON_GetObjectItem(
                    json, "sender")->valuestring;
                const char *rec = cJSON_GetObjectItem(
                    json, "recipient")->valuestring;
                int ts = cJSON_GetObjectItem(
                    json, "timestamp")->valueint;

                const char *ekf = (strcmp(username, snd) == 0)
                    ? "enc_key_sender" : "enc_key_recipient";
                const char *enc_key = cJSON_GetObjectItem(
                    json, ekf)->valuestring;
                const char *enc_sub = cJSON_GetObjectItem(
                    json, "enc_subject")->valuestring;
                const char *enc_bdy = cJSON_GetObjectItem(
                    json, "enc_body")->valuestring;

                char path[512];
                size_t ksk_len;
                snprintf(path, sizeof(path), "%s/kem_sk.bin",
                         ctx->config_dir);
                uint8_t *ksk = load_file(path, &ksk_len);

                char *sub = NULL, *bdy = NULL;
                if (ksk) {
                    uint8_t *sym = kem_decapsulate_key(enc_key, ksk);
                    if (sym) {
                        sub = decrypt_from_b64(sym, enc_sub);
                        bdy = decrypt_from_b64(sym, enc_bdy);
                        free(sym);
                    }
                    free(ksk);
                }

                result = calloc(1, sizeof(shyake_mail_detail));
                result->mail_id   = strdup(mail_id);
                result->sender    = strdup(snd);
                result->recipient = strdup(rec);
                result->subject   = sub;
                result->body      = bdy;
                result->timestamp = (int64_t)ts;
                result->size      = 0;

                cJSON_Delete(json);
            }
        } else {
            fprintf(stderr, "Failed to fetch mail (HTTP %ld): %s\n",
                    http_code, resp.data);
        }
    } else {
        fprintf(stderr, "Network error: %s\n",
                curl_easy_strerror(res));
    }

    free(resp.data);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return result;
}

/* ------------------------------------------------------------------ */

shyake_mail_detail*
shyake_check_one(shyake_ctx *ctx, const char *mail_id)
{
    if (!ctx || !mail_id) return NULL;
    const char *username = ctx->username;

    char endpoint[128];
    snprintf(endpoint, sizeof(endpoint), "/api/mail/%s", mail_id);
    char url[512];
    snprintf(url, sizeof(url), "%s%s", ctx->instance_url, endpoint);

    CURL *curl = curl_easy_init();
    if (!curl) return NULL;

    if (ctx->debug)
        curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L);

    struct curl_slist *headers = create_auth_headers(
        ctx, endpoint, username);
    if (!headers) { curl_easy_cleanup(curl); return NULL; }

    struct curl_response resp = { .data = malloc(1), .size = 0 };
    resp.data[0] = '\0';

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&resp);

    CURLcode res = curl_easy_perform(curl);
    shyake_mail_detail *result = NULL;

    if (res == CURLE_OK) {
        long http_code;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
        if (http_code == 200) {
            cJSON *json = cJSON_Parse(resp.data);
            if (json) {
                const char *snd = cJSON_GetObjectItem(
                    json, "sender")->valuestring;
                const char *rec = cJSON_GetObjectItem(
                    json, "recipient")->valuestring;
                int ts = cJSON_GetObjectItem(
                    json, "timestamp")->valueint;
                int sz = cJSON_GetObjectItem(
                    json, "size")->valueint;

                const char *ekf = (strcmp(username, snd) == 0)
                    ? "enc_key_sender" : "enc_key_recipient";
                const char *enc_key = cJSON_GetObjectItem(
                    json, ekf)->valuestring;
                const char *enc_sub = cJSON_GetObjectItem(
                    json, "enc_subject")->valuestring;

                char path[512];
                size_t ksk_len;
                snprintf(path, sizeof(path), "%s/kem_sk.bin",
                         ctx->config_dir);
                uint8_t *ksk = load_file(path, &ksk_len);
                char *sub = NULL;
                if (ksk) {
                    uint8_t *sym = kem_decapsulate_key(enc_key, ksk);
                    if (sym) {
                        sub = decrypt_from_b64(sym, enc_sub);
                        free(sym);
                    }
                    free(ksk);
                }

                result = calloc(1, sizeof(shyake_mail_detail));
                result->mail_id   = strdup(mail_id);
                result->sender    = strdup(snd);
                result->recipient = strdup(rec);
                result->subject   = sub;
                result->body      = NULL;
                result->timestamp = (int64_t)ts;
                result->size      = sz;

                cJSON_Delete(json);
            }
        } else if (http_code == 404) {
            fprintf(stderr, "Mail not found.\n");
        } else {
            fprintf(stderr, "Failed (HTTP %ld): %s\n",
                    http_code, resp.data);
        }
    } else {
        fprintf(stderr, "Network error: %s\n",
                curl_easy_strerror(res));
    }

    free(resp.data);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return result;
}

/* ------------------------------------------------------------------ */

shyake_err
shyake_burn(shyake_ctx *ctx, const char *mail_id)
{
    if (!ctx || !mail_id) return SHYAKE_ERR;
    const char *username = ctx->username;

    char endpoint[128];
    snprintf(endpoint, sizeof(endpoint), "/api/mail/%s", mail_id);
    char url[512];
    snprintf(url, sizeof(url), "%s%s", ctx->instance_url, endpoint);

    CURL *curl = curl_easy_init();
    if (!curl) return SHYAKE_ERR;

    if (ctx->debug)
        curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L);

    struct curl_slist *headers = create_signed_headers(
        ctx, "DELETE", endpoint, username);
    if (!headers) { curl_easy_cleanup(curl); return SHYAKE_ERR; }

    struct curl_response resp = { .data = malloc(1), .size = 0 };
    resp.data[0] = '\0';

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "DELETE");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&resp);

    CURLcode res = curl_easy_perform(curl);
    shyake_err ret = SHYAKE_OK;

    if (res == CURLE_OK) {
        long http_code;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
        if (http_code == 200) {
            ret = SHYAKE_OK;
        } else if (http_code == 404) {
            ret = SHYAKE_ERR_NOT_FOUND;
        } else if (http_code == 403) {
            ret = SHYAKE_ERR_FORBIDDEN;
        } else {
            ret = SHYAKE_ERR_HTTP;
        }
    } else {
        ret = SHYAKE_ERR_NETWORK;
    }

    free(resp.data);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return ret;
}
