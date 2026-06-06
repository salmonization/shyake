#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <oqs/oqs.h>
#include <curl/curl.h>
#include "vendor/cJSON/cJSON.h"
#include "lib_internal.h"

shyake_err
shyake_block(shyake_ctx *ctx, const char *target, int unblock)
{
    if (!ctx || !target) return SHYAKE_ERR;
    const char *username = ctx->username;
    const char *method = unblock ? "DELETE" : "POST";
    const char *endpoint = "/api/block";

    char url[512];
    snprintf(url, sizeof(url), "%s%s", ctx->instance_url, endpoint);

    CURL *curl = curl_easy_init();
    if (!curl) return SHYAKE_ERR;

    struct curl_slist *headers = create_signed_headers(
        ctx, method, endpoint, username);
    if (!headers) { curl_easy_cleanup(curl); return SHYAKE_ERR; }
    headers = curl_slist_append(headers, "Content-Type: application/json");

    cJSON *body_json = cJSON_CreateObject();
    cJSON_AddStringToObject(body_json, "target", target);
    char *body_str = cJSON_PrintUnformatted(body_json);
    cJSON_Delete(body_json);

    struct curl_response resp = { .data = malloc(1), .size = 0 };
    resp.data[0] = '\0';

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body_str);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&resp);

    CURLcode res = curl_easy_perform(curl);
    shyake_err ret = SHYAKE_OK;

    if (res == CURLE_OK) {
        long http_code;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
        if (http_code == 200 || http_code == 201) {
            ret = SHYAKE_OK;
        } else {
            ret = SHYAKE_ERR_HTTP;
        }
    } else {
        ret = SHYAKE_ERR_NETWORK;
    }

    free(body_str);
    free(resp.data);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return ret;
}

shyake_err
shyake_rotate(shyake_ctx *ctx)
{
    if (!ctx) return SHYAKE_ERR;
    const char *username = ctx->username;

    OQS_KEM *kem = OQS_KEM_new("ML-KEM-768");
    OQS_SIG *sig = OQS_SIG_new("ML-DSA-65");
    if (!kem || !sig) {
        if (kem) OQS_KEM_free(kem);
        if (sig) OQS_SIG_free(sig);
        return SHYAKE_ERR_CRYPTO;
    }

    uint8_t *new_kpk = malloc(kem->length_public_key);
    uint8_t *new_ksk = malloc(kem->length_secret_key);
    uint8_t *new_spk = malloc(sig->length_public_key);
    uint8_t *new_ssk = malloc(sig->length_secret_key);

    if (OQS_KEM_keypair(kem, new_kpk, new_ksk) != OQS_SUCCESS ||
        OQS_SIG_keypair(sig, new_spk, new_ssk) != OQS_SUCCESS) {
        free(new_kpk); free(new_ksk); free(new_spk); free(new_ssk);
        OQS_KEM_free(kem); OQS_SIG_free(sig);
        return SHYAKE_ERR_CRYPTO;
    }

    char *kpk_b64 = base64_encode(new_kpk, kem->length_public_key);
    char *spk_b64 = base64_encode(new_spk, sig->length_public_key);

    char endpoint[128];
    snprintf(endpoint, sizeof(endpoint), "/api/rotate");
    char url[512];
    snprintf(url, sizeof(url), "%s%s", ctx->instance_url, endpoint);

    CURL *curl = curl_easy_init();
    if (!curl) {
        free(new_kpk); free(new_ksk); free(new_spk); free(new_ssk);
        free(kpk_b64); free(spk_b64);
        OQS_KEM_free(kem); OQS_SIG_free(sig);
        return SHYAKE_ERR;
    }

    /* create_signed_headers uses the OLD private key currently on disk */
    struct curl_slist *headers = create_signed_headers(
        ctx, "POST", endpoint, username);
    if (!headers) {
        curl_easy_cleanup(curl);
        free(new_kpk); free(new_ksk); free(new_spk); free(new_ssk);
        free(kpk_b64); free(spk_b64);
        OQS_KEM_free(kem); OQS_SIG_free(sig);
        return SHYAKE_ERR;
    }
    headers = curl_slist_append(headers, "Content-Type: application/json");

    cJSON *body_json = cJSON_CreateObject();
    cJSON_AddStringToObject(body_json, "new_kem_pubkey", kpk_b64);
    cJSON_AddStringToObject(body_json, "new_sig_pubkey", spk_b64);
    char *body_str = cJSON_PrintUnformatted(body_json);
    cJSON_Delete(body_json);

    struct curl_response resp = { .data = malloc(1), .size = 0 };
    resp.data[0] = '\0';

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "POST");
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body_str);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&resp);

    CURLcode res = curl_easy_perform(curl);
    shyake_err ret = SHYAKE_OK;

    if (res == CURLE_OK) {
        long http_code;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
        if (http_code == 200 || http_code == 201) {
            /* save new keys locally only after server confirmed */
            char path[512];
            snprintf(path, sizeof(path), "%s/kem_pk.bin", ctx->config_dir);
            save_file(path, new_kpk, kem->length_public_key);
            snprintf(path, sizeof(path), "%s/kem_sk.bin", ctx->config_dir);
            save_file(path, new_ksk, kem->length_secret_key);
            snprintf(path, sizeof(path), "%s/sig_pk.bin", ctx->config_dir);
            save_file(path, new_spk, sig->length_public_key);
            snprintf(path, sizeof(path), "%s/sig_sk.bin", ctx->config_dir);
            save_file(path, new_ssk, sig->length_secret_key);
        } else {
            ret = SHYAKE_ERR_HTTP;
        }
    } else {
        ret = SHYAKE_ERR_NETWORK;
    }

    free(body_str);
    free(resp.data);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    free(new_kpk); free(new_ksk); free(new_spk); free(new_ssk);
    free(kpk_b64); free(spk_b64);
    OQS_KEM_free(kem); OQS_SIG_free(sig);
    return ret;
}

shyake_err
shyake_destroy(shyake_ctx *ctx)
{
    if (!ctx) return SHYAKE_ERR;
    const char *username = ctx->username;

    char endpoint[128];
    snprintf(endpoint, sizeof(endpoint), "/api/destroy");
    char url[512];
    snprintf(url, sizeof(url), "%s%s", ctx->instance_url, endpoint);

    CURL *curl = curl_easy_init();
    if (!curl) return SHYAKE_ERR;

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
