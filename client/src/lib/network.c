#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <oqs/oqs.h>
#include <openssl/sha.h>
#include <curl/curl.h>
#include "vendor/cJSON/cJSON.h"
#include "lib_internal.h"

size_t
curl_write_cb(void *contents, size_t size, size_t nmemb, void *userp)
{
    size_t realsize = size * nmemb;
    struct curl_response *mem = (struct curl_response *)userp;
    char *ptr = realloc(mem->data, mem->size + realsize + 1);
    if (!ptr) return 0;
    mem->data = ptr;
    memcpy(&(mem->data[mem->size]), contents, realsize);
    mem->size += realsize;
    mem->data[mem->size] = 0;
    return realsize;
}

struct curl_slist*
create_signed_headers(shyake_ctx *ctx, const char *method,
                      const char *endpoint, const char *username)
{
    // sign request with given method and mint PoW
    time_t now = time(NULL);
    char timestamp[32];
    snprintf(timestamp, sizeof(timestamp), "%ld", now);

    char path[512];
    size_t ssk_len;
    snprintf(path, sizeof(path), "%s/sig_sk.bin", ctx->config_dir);
    uint8_t *ssk = load_file(path, &ssk_len);
    if (!ssk)
        return NULL;

    char message[512];
    snprintf(message, sizeof(message), "%s:%s:%s:%s",
             method, endpoint, username, timestamp);

    OQS_SIG *sig = OQS_SIG_new("ML-DSA-65");
    if (!sig) { free(ssk); return NULL; }
    uint8_t *signature = malloc(sig->length_signature);
    size_t sig_len;
    OQS_SIG_sign(sig, signature, &sig_len, (uint8_t*)message,
                 strlen(message), ssk);
    char *sig_b64 = base64_encode(signature, sig_len);

    char *pow = shyake_mint_pow(username, 20);

    struct curl_slist *headers = NULL;
    char header_buf[8192];
    snprintf(header_buf, sizeof(header_buf),
             "X-Shyake-Username: %s", username);
    headers = curl_slist_append(headers, header_buf);
    snprintf(header_buf, sizeof(header_buf),
             "X-Shyake-Timestamp: %s", timestamp);
    headers = curl_slist_append(headers, header_buf);
    snprintf(header_buf, sizeof(header_buf),
             "X-Shyake-Signature: %s", sig_b64);
    headers = curl_slist_append(headers, header_buf);
    snprintf(header_buf, sizeof(header_buf),
             "X-Shyake-Pow: %s", pow);
    headers = curl_slist_append(headers, header_buf);

    free(ssk);
    free(signature);
    free(sig_b64);
    free(pow);
    OQS_SIG_free(sig);
    return headers;
}

struct curl_slist*
create_auth_headers(shyake_ctx *ctx, const char *endpoint,
                    const char *username)
{
    return create_signed_headers(ctx, "GET", endpoint, username);
}

char*
fetch_recipient_pubkey(shyake_ctx *ctx, const char *recipient)
{
    // fetch public key for recipient from server
    char url[512];
    snprintf(url, sizeof(url), "%s/api/pubkey/%s",
             ctx->instance_url, recipient);

    CURL *curl = curl_easy_init();
    if (!curl) return NULL;

    struct curl_response resp = { .data = malloc(1), .size = 0 };
    resp.data[0] = '\0';

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&resp);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_cleanup(curl);

    if (res == CURLE_OK && http_code == 200) {
        cJSON *json = cJSON_Parse(resp.data);
        free(resp.data);
        if (json) {
            cJSON *kem_pk_item = cJSON_GetObjectItem(json, "kem_pubkey");
            char *kem_pk = NULL;
            if (cJSON_IsString(kem_pk_item))
                kem_pk = strdup(kem_pk_item->valuestring);
            cJSON_Delete(json);
            return kem_pk;
        }
        return NULL;
    } else if (res != CURLE_OK) {
        fprintf(stderr, "Network error: %s\n",
                curl_easy_strerror(res));
    }

    free(resp.data);
    return NULL;
}
