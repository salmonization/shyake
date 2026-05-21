#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#include <time.h>
#include <oqs/oqs.h>
#include <openssl/sha.h>
#include <openssl/evp.h>
#include <curl/curl.h>
#include "vendor/cJSON/cJSON.h"

#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/wait.h>

#include "internal.h"
#include "shyake_crypto.h"

static int pager_pid = -1;
static int pager_fd = -1;
static int saved_stdout = -1;

static void
setup_pager(int disable_pager)
{
    // spawn less pager if stdout is TTY
    if (disable_pager || !isatty(STDOUT_FILENO))
        return;

    int fd[2];
    if (pipe(fd) < 0)
        return;

    pager_pid = fork();
    if (pager_pid < 0) {
        close(fd[0]);
        close(fd[1]);
        return;
    }

    if (pager_pid == 0) {
        dup2(fd[0], STDIN_FILENO);
        close(fd[0]);
        close(fd[1]);
        execlp("less", "less", "-F", "-R", "-X", NULL);
        exit(1);
    } else {
        saved_stdout = dup(STDOUT_FILENO);
        dup2(fd[1], STDOUT_FILENO);
        close(fd[0]);
        close(fd[1]);
        pager_fd = STDOUT_FILENO;
    }
}

static void
wait_pager(void)
{
    // wait for less pager process
    if (pager_pid > 0) {
        fflush(stdout);
        close(STDOUT_FILENO);
        if (saved_stdout >= 0) {
            dup2(saved_stdout, STDOUT_FILENO);
            close(saved_stdout);
            saved_stdout = -1;
        }
        waitpid(pager_pid, NULL, 0);
        pager_pid = -1;
    }
}

static int
get_terminal_width(void)
{
    // get current terminal width
    struct winsize w;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == 0 && w.ws_col > 0)
        return w.ws_col;
    return 80;
}

static char*
format_sender(const char *sender)
{
    // format sender for displaying
    char *dup = strdup(sender);
    char *at = strchr(dup, '@');
    if (at)
        at[1] = '\0';
    return dup;
}

static void
format_size(char *buf, size_t buf_len, int size)
{
    // format byte size to human readable
    if (size >= 1024)
        snprintf(buf, buf_len, "%dK", size / 1024);
    else
        snprintf(buf, buf_len, "%d", size);
}

static void
print_word_wrap(const char *text, int indent, int width)
{
    // word-wrap text with aligned continuation indent
    int avail = width - indent;
    if (avail < 20)
        avail = 20;

    int len = (int)strlen(text);
    int pos = 0;
    int first = 1;

    while (pos < len) {
        if (!first)
            printf("\n%*s", indent, "");
        first = 0;

        int remain = len - pos;
        if (remain <= avail) {
            printf("%.*s", remain, text + pos);
            pos += remain;
        } else {
            /* find last space within avail */
            int cut = avail;
            while (cut > 0 && text[pos + cut] != ' ')
                cut--;
            if (cut == 0)
                cut = avail;
            printf("%.*s", cut, text + pos);
            pos += cut;
            while (pos < len && text[pos] == ' ')
                pos++;
        }
    }
    printf("\n");
}

static int save_file(const char *path, const uint8_t *data, size_t len)
{
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    size_t written = fwrite(data, 1, len, f);
    fclose(f);
    return written == len ? 0 : -1;
}

static uint8_t* load_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    *len = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *data = malloc(*len);
    if (data) {
        if (fread(data, 1, *len, f) != *len) {
            free(data);
            data = NULL;
        }
    }
    fclose(f);
    return data;
}

static char* base64_encode(const uint8_t *data, size_t len)
{
    int out_len = 4 * ((len + 2) / 3);
    char *out = malloc(out_len + 1);
    if (!out) return NULL;
    EVP_EncodeBlock((unsigned char *)out, data, len);
    return out;
}

shyake_ctx*
shyake_init_ctx(const shyake_config *config)
{
    // initialize context
    if (!config || !config->config_dir)
        return NULL;

    shyake_ctx *ctx = calloc(1, sizeof(shyake_ctx));
    if (!ctx)
        return NULL;

    ctx->config_dir = strdup(config->config_dir);
    if (config->instance_url)
        ctx->instance_url = strdup(config->instance_url);
    if (config->username)
        ctx->username = strdup(config->username);
    if (config->time_format)
        ctx->time_format = strdup(config->time_format);
    if (config->time_format_recent)
        ctx->time_format_recent = strdup(config->time_format_recent);
    if (config->check_columns)
        ctx->check_columns = strdup(config->check_columns);

    ctx->plain = config->plain;
    ctx->debug = config->debug;
    ctx->no_color = config->no_color;

    curl_global_init(CURL_GLOBAL_ALL);
    return ctx;
}

void
shyake_free_ctx(shyake_ctx *ctx)
{
    // free context
    if (!ctx)
        return;
    free(ctx->config_dir);
    free(ctx->instance_url);
    free(ctx->username);
    free(ctx->time_format);
    free(ctx->time_format_recent);
    free(ctx->check_columns);
    free(ctx);
    curl_global_cleanup();
}

int shyake_generate_keys(shyake_ctx *ctx)
{
    if (!ctx || !ctx->config_dir) return -1;

    char path_pk[512], path_sk[512];
    struct stat st = {0};
    int ret = 0;

    snprintf(path_pk, sizeof(path_pk), "%s/kem_pk.bin", ctx->config_dir);
    if (stat(path_pk, &st) == -1) {
        OQS_KEM *kem = OQS_KEM_new("ML-KEM-768");
        if (kem) {
            uint8_t *pk = malloc(kem->length_public_key);
            uint8_t *sk = malloc(kem->length_secret_key);
            if (OQS_KEM_keypair(kem, pk, sk) == OQS_SUCCESS) {
                snprintf(path_sk, sizeof(path_sk), "%s/kem_sk.bin",
                         ctx->config_dir);
                save_file(path_pk, pk, kem->length_public_key);
                save_file(path_sk, sk, kem->length_secret_key);
            } else ret = -1;
            free(pk); free(sk);
            OQS_KEM_free(kem);
        } else ret = -1;
    }

    snprintf(path_pk, sizeof(path_pk), "%s/sig_pk.bin", ctx->config_dir);
    if (stat(path_pk, &st) == -1) {
        OQS_SIG *sig = OQS_SIG_new("ML-DSA-65");
        if (sig) {
            uint8_t *pk = malloc(sig->length_public_key);
            uint8_t *sk = malloc(sig->length_secret_key);
            if (OQS_SIG_keypair(sig, pk, sk) == OQS_SUCCESS) {
                snprintf(path_sk, sizeof(path_sk), "%s/sig_sk.bin",
                         ctx->config_dir);
                save_file(path_pk, pk, sig->length_public_key);
                save_file(path_sk, sk, sig->length_secret_key);
            } else ret = -1;
            free(pk); free(sk);
            OQS_SIG_free(sig);
        } else ret = -1;
    }
    return ret;
}

char* shyake_mint_pow(const char *resource, int bits)
{
    time_t now = time(NULL);
    struct tm *tm_info = gmtime(&now);
    char date[7];
    strftime(date, 7, "%y%m%d", tm_info);

    char rand_str[13];
    const char charset[] = "abcdefghijklmnopqrstuvwxyz0123456789";
    for (int i = 0; i < 12; i++) rand_str[i] = charset[rand() % 36];
    rand_str[12] = '\0';

    char *header = malloc(256);
    unsigned long counter = 0;
    unsigned char hash[SHA_DIGEST_LENGTH];
    int bytes_to_check = bits / 8;
    int remaining_bits = bits % 8;

    while (1) {
        sprintf(header, "1:%d:%s:%s::%s:%lx", bits, date,
                resource, rand_str, counter);
        SHA1((unsigned char *)header, strlen(header), hash);
        int match = 1;
        for (int i = 0; i < bytes_to_check; i++) {
            if (hash[i] != 0) { match = 0; break; }
        }
        if (match && remaining_bits > 0) {
            if ((hash[bytes_to_check] >> (8 - remaining_bits)) != 0) {
                match = 0;
            }
        }
        if (match) return header;
        counter++;
        if (counter == 0xFFFFFFFF) break;
    }
    free(header);
    return NULL;
}

struct curl_response {
    char *data;
    size_t size;
};

static size_t curl_write_cb(void *contents, size_t size, size_t nmemb,
                            void *userp)
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

int shyake_register(shyake_ctx *ctx, const char *username)
{
    if (!ctx || !username) return -1;

    size_t kpk_len, spk_len, ssk_len;
    char path[512];
    
    snprintf(path, sizeof(path), "%s/kem_pk.bin", ctx->config_dir);
    uint8_t *kpk = load_file(path, &kpk_len);
    snprintf(path, sizeof(path), "%s/sig_pk.bin", ctx->config_dir);
    uint8_t *spk = load_file(path, &spk_len);
    snprintf(path, sizeof(path), "%s/sig_sk.bin", ctx->config_dir);
    uint8_t *ssk = load_file(path, &ssk_len);

    if (!kpk || !spk || !ssk) {
        free(kpk); free(spk); free(ssk);
        return -1;
    }

    char *kpk_b64 = base64_encode(kpk, kpk_len);
    char *spk_b64 = base64_encode(spk, spk_len);
    
    time_t now = time(NULL);
    char timestamp[32];
    snprintf(timestamp, sizeof(timestamp), "%ld", now);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "username", username);
    cJSON_AddStringToObject(root, "kem_pubkey", kpk_b64);
    cJSON_AddStringToObject(root, "sig_pubkey", spk_b64);
    cJSON_AddStringToObject(root, "timestamp", timestamp);

    char *json_raw = cJSON_PrintUnformatted(root);
    
    /* Sign the JSON payload */
    OQS_SIG *sig = OQS_SIG_new("ML-DSA-65");
    uint8_t *signature = malloc(sig->length_signature);
    size_t sig_len;
    OQS_SIG_sign(sig, signature, &sig_len, (uint8_t*)json_raw,
                 strlen(json_raw), ssk);
    char *sig_b64 = base64_encode(signature, sig_len);

    /* Mint PoW */
    char *pow = shyake_mint_pow(username, 20);

    /* Final Payload */
    cJSON_AddStringToObject(root, "signature", sig_b64);
    cJSON_AddStringToObject(root, "pow", pow);
    char *payload = cJSON_PrintUnformatted(root);

    int ret = 0;
    if (ctx->instance_url) {
        CURL *curl = curl_easy_init();
        if (curl) {
            char url[512];
            snprintf(url, sizeof(url), "%s/api/register",
                     ctx->instance_url);
            
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
            /* TODO: Remove this 10s timeout before release to support 
             * poor networks (let user Ctrl+C) */
            curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);

            printf("Registering as %s at %s...\n", username,
                   ctx->instance_url);
            CURLcode res = curl_easy_perform(curl);
            if (res != CURLE_OK) {
                fprintf(stderr, "curl_easy_perform() failed: %s\n",
                        curl_easy_strerror(res));
                ret = -1;
            } else {
                long http_code = 0;
                curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
                if (http_code == 200 || http_code == 201) {
                    printf("Successfully registered.\n");
                } else {
                    fprintf(stderr, "Registration failed (HTTP %ld): %s\n",
                            http_code, resp.data);
                    ret = -1;
                }
            }
            
            free(resp.data);
            curl_slist_free_all(headers);
            curl_easy_cleanup(curl);
        }
    } else {
        fprintf(stderr, "Error: Instance URL not provided.\n");
        ret = -1;
    }

    /* Cleanup */
    free(json_raw); free(payload); free(signature); free(sig_b64); 
    free(pow); free(kpk_b64); free(spk_b64);
    free(kpk); free(spk); free(ssk);
    cJSON_Delete(root);
    OQS_SIG_free(sig);

    return ret;
}

static char* encrypt_to_b64(const uint8_t *key, const uint8_t *pt,
                            size_t pt_len)
{
    if (!pt || pt_len == 0) return NULL;
    
    uint8_t nonce[12];
    FILE *urandom = fopen("/dev/urandom", "rb");
    if (urandom) {
        fread(nonce, 1, 12, urandom);
        fclose(urandom);
    } else return NULL;

    uint8_t *ct = malloc(pt_len);
    uint8_t mac[16];
    
    if (chacha20_poly1305_encrypt(key, nonce, pt, pt_len, NULL, 0, ct,
                                  mac) != 0) {
        free(ct);
        return NULL;
    }
    
    size_t packed_len = 12 + pt_len + 16;
    uint8_t *packed = malloc(packed_len);
    memcpy(packed, nonce, 12);
    memcpy(packed + 12, ct, pt_len);
    memcpy(packed + 12 + pt_len, mac, 16);
    
    char *b64 = base64_encode(packed, packed_len);
    free(ct); free(packed);
    return b64;
}

static char* fetch_recipient_pubkey(shyake_ctx *ctx, const char *recipient)
{
    char url[512];
    snprintf(url, sizeof(url), "%s/api/pubkey/%s", ctx->instance_url,
             recipient);
    
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
            if (cJSON_IsString(kem_pk_item)) {
                kem_pk = strdup(kem_pk_item->valuestring);
            }
            cJSON_Delete(json);
            return kem_pk;
        }
        return NULL;
    }
    
    free(resp.data);
    return NULL;
}

static uint8_t*
base64_decode(const char *b64, size_t *out_len)
{
    size_t len = strlen(b64);
    while (len > 0 && (b64[len - 1] == '\r' ||
                       b64[len - 1] == '\n' ||
                       b64[len - 1] == ' '))
        len--;

    uint8_t *out = malloc(len);
    if (!out)
        return NULL;

    char *std_b64 = strdup(b64);
    for (size_t i = 0; i < len; i++) {
        if (std_b64[i] == '-')
            std_b64[i] = '+';
        if (std_b64[i] == '_')
            std_b64[i] = '/';
    }

    int dec_len = EVP_DecodeBlock(out, (const unsigned char *)std_b64, len);
    free(std_b64);

    if (dec_len < 0) {
        free(out);
        return NULL;
    }

    int padding = 0;
    if (len > 0 && b64[len - 1] == '=') {
        padding++;
        if (len > 1 && b64[len - 2] == '=')
            padding++;
    } else {
        int mod = len % 4;
        if (mod == 2)
            padding = 2;
        else if (mod == 3)
            padding = 1;
    }
    dec_len -= padding;

    *out_len = dec_len;
    return out;
}

static char* kem_encapsulate_key(const uint8_t *kem_pk,
                                 const uint8_t *sym_key)
{
    OQS_KEM *kem = OQS_KEM_new("ML-KEM-768");
    if (!kem) return NULL;
    
    uint8_t *ct = malloc(kem->length_ciphertext);
    uint8_t *ss = malloc(kem->length_shared_secret);
    
    if (OQS_KEM_encaps(kem, ct, ss, kem_pk) != OQS_SUCCESS) {
        free(ct); free(ss); OQS_KEM_free(kem);
        return NULL;
    }
    
    /* XOR the symmetric key with the shared secret */
    uint8_t ek[32];
    for (int i = 0; i < 32; i++) {
        ek[i] = sym_key[i] ^ ss[i];
    }
    
    /* Packed: CT || EK */
    size_t packed_len = kem->length_ciphertext + 32;
    uint8_t *packed = malloc(packed_len);
    memcpy(packed, ct, kem->length_ciphertext);
    memcpy(packed + kem->length_ciphertext, ek, 32);
    
    char *b64 = base64_encode(packed, packed_len);
    
    free(ct); free(ss); free(packed); OQS_KEM_free(kem);
    return b64;
}

int shyake_send(shyake_ctx *ctx, const char *recipient,
                const char *subject, const uint8_t *body,
                size_t body_len)
{
    if (!ctx || !recipient || !body || body_len == 0) return -1;
    
    printf("Fetching public key for %s...\n", recipient);
    char *recip_pk_b64 = fetch_recipient_pubkey(ctx, recipient);
    if (!recip_pk_b64) {
        fprintf(stderr, "Failed to fetch recipient public key.\n");
        return -1;
    }
    
    size_t recip_pk_len;
    uint8_t *recip_pk = base64_decode(recip_pk_b64, &recip_pk_len);
    if (!recip_pk) {
        free(recip_pk_b64);
        return -1;
    }
    
    /* Load sender keys */
    size_t my_kpk_len, my_ssk_len;
    char path[512];
    snprintf(path, sizeof(path), "%s/kem_pk.bin", ctx->config_dir);
    uint8_t *my_kpk = load_file(path, &my_kpk_len);
    snprintf(path, sizeof(path), "%s/sig_sk.bin", ctx->config_dir);
    uint8_t *my_ssk = load_file(path, &my_ssk_len);
    
    if (!my_kpk || !my_ssk) {
        free(recip_pk_b64); free(recip_pk); free(my_kpk); free(my_ssk);
        return -1;
    }
    
    /* Calculate recipient fingerprint (SHA256 of KEM PK) */
    unsigned char fingerprint[SHA256_DIGEST_LENGTH];
    SHA256(recip_pk, recip_pk_len, fingerprint);
    char fp_hex[SHA256_DIGEST_LENGTH * 2 + 1];
    for (int i = 0; i < SHA256_DIGEST_LENGTH; i++) {
        sprintf(fp_hex + (i * 2), "%02x", fingerprint[i]);
    }
    
    /* Generate 32-byte symmetric message key */
    uint8_t sym_key[32];
    FILE *urandom = fopen("/dev/urandom", "rb");
    if (urandom) {
        fread(sym_key, 1, 32, urandom);
        fclose(urandom);
    } else {
        free(recip_pk_b64); free(recip_pk); free(my_kpk); free(my_ssk);
        return -1;
    }
    
    /* Encrypt subject and body */
    char *enc_subject = encrypt_to_b64(sym_key, (const uint8_t*)subject,
                                       subject ? strlen(subject) : 0);
    char *enc_body = encrypt_to_b64(sym_key, body, body_len);
    
    /* Encapsulate key for recipient and sender */
    char *enc_key_recipient = kem_encapsulate_key(recip_pk, sym_key);
    char *enc_key_sender = kem_encapsulate_key(my_kpk, sym_key);
    
    time_t now = time(NULL);
    char timestamp[32];
    snprintf(timestamp, sizeof(timestamp), "%ld", now);
    
    /* TODO: implement the sender name logic properly (requires reading 
     * config or local state for the username) */
    const char *sender = ctx->username ? ctx->username : "salmon";
    
    cJSON *signed_obj = cJSON_CreateObject();
    cJSON_AddStringToObject(signed_obj, "sender", sender);
    cJSON_AddStringToObject(signed_obj, "recipient", recipient);
    cJSON_AddStringToObject(signed_obj, "recipient_kem_fingerprint",
                            fp_hex);
    if (enc_subject) {
        cJSON_AddStringToObject(signed_obj, "enc_subject", enc_subject);
    } else {
        cJSON_AddStringToObject(signed_obj, "enc_subject", "");
    }
    cJSON_AddStringToObject(signed_obj, "enc_body", enc_body);
    cJSON_AddStringToObject(signed_obj, "timestamp", timestamp);
    cJSON_AddNumberToObject(signed_obj, "size", body_len);

    char *json_raw = cJSON_PrintUnformatted(signed_obj);
    
    /* Sign the JSON payload */
    OQS_SIG *sig = OQS_SIG_new("ML-DSA-65");
    uint8_t *signature = malloc(sig->length_signature);
    size_t sig_len;
    OQS_SIG_sign(sig, signature, &sig_len, (uint8_t*)json_raw,
                 strlen(json_raw), my_ssk);
    char *sig_b64 = base64_encode(signature, sig_len);
    
    /* Mint PoW */
    char *pow = shyake_mint_pow(sender, 20);
    
    /* Final Payload */
    cJSON *root = cJSON_Duplicate(signed_obj, 1);
    cJSON_AddStringToObject(root, "enc_key_sender", enc_key_sender);
    cJSON_AddStringToObject(root, "enc_key_recipient", enc_key_recipient);
    cJSON_AddStringToObject(root, "signature", sig_b64);
    cJSON_AddStringToObject(root, "pow", pow);
    char *payload = cJSON_PrintUnformatted(root);
    
    printf("Sending mail to %s...\n", recipient);
    
    int ret = 0;
    CURL *curl = curl_easy_init();
    if (curl) {
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
        /* TODO: Remove this 10s timeout before release to support 
         * poor networks (let user Ctrl+C) */
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);

        CURLcode res = curl_easy_perform(curl);
        if (res != CURLE_OK) {
            fprintf(stderr, "curl_easy_perform() failed: %s\n",
                    curl_easy_strerror(res));
            ret = -1;
        } else {
            long http_code = 0;
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
            if (http_code == 200 || http_code == 201) {
                printf("Your mail was sent.\n");
            } else {
                fprintf(stderr, "Send failed (HTTP %ld): %s\n",
                        http_code, resp.data);
                ret = -1;
            }
        }
        
        free(resp.data);
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
    } else {
        fprintf(stderr, "Error: Instance URL not provided.\n");
        ret = -1;
    }
    
    /* Cleanup */
    free(recip_pk_b64); free(recip_pk); free(my_kpk); free(my_ssk);
    free(enc_subject); free(enc_body);
    free(enc_key_recipient); free(enc_key_sender);
    free(json_raw); free(payload); free(signature);
    free(sig_b64); free(pow);
    cJSON_Delete(root);
    cJSON_Delete(signed_obj);
    OQS_SIG_free(sig);
    
    return ret;
}

static uint8_t* kem_decapsulate_key(const char *enc_key_b64,
                                    const uint8_t *my_sk)
{
    size_t packed_len;
    uint8_t *packed = base64_decode(enc_key_b64, &packed_len);
    if (!packed) return NULL;
    
    OQS_KEM *kem = OQS_KEM_new("ML-KEM-768");
    if (!kem) { free(packed); return NULL; }
    
    if (packed_len != kem->length_ciphertext + 32) {
        free(packed); OQS_KEM_free(kem); return NULL;
    }
    
    uint8_t *ss = malloc(kem->length_shared_secret);
    if (OQS_KEM_decaps(kem, ss, packed, my_sk) != OQS_SUCCESS) {
        free(ss); free(packed); OQS_KEM_free(kem); return NULL;
    }
    
    uint8_t *sym_key = malloc(32);
    uint8_t *ek = packed + kem->length_ciphertext;
    for (int i = 0; i < 32; i++) {
        sym_key[i] = ek[i] ^ ss[i];
    }
    
    free(ss); free(packed); OQS_KEM_free(kem);
    return sym_key;
}

static char* decrypt_from_b64(const uint8_t *key, const char *b64)
{
    if (!b64 || strlen(b64) == 0) return NULL;
    size_t packed_len;
    uint8_t *packed = base64_decode(b64, &packed_len);
    if (!packed || packed_len <= 28) { free(packed); return NULL; }
    
    size_t ct_len = packed_len - 28;
    uint8_t nonce[12];
    uint8_t mac[16];
    uint8_t *ct = packed + 12;
    memcpy(nonce, packed, 12);
    memcpy(mac, packed + 12 + ct_len, 16);
    
    char *pt = malloc(ct_len + 1);
    if (chacha20_poly1305_decrypt(key, nonce, ct, ct_len, NULL, 0, mac,
                                  (uint8_t*)pt) != 0) {
        free(pt); free(packed); return NULL;
    }
    pt[ct_len] = '\0';
    free(packed);
    return pt;
}

static struct curl_slist*
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

static struct curl_slist*
create_auth_headers(shyake_ctx *ctx, const char *endpoint,
                    const char *username)
{
    return create_signed_headers(ctx, "GET", endpoint, username);
}

int shyake_check(shyake_ctx *ctx, const char *type)
{
    if (!ctx || !type)
        return -1;
    const char *username = ctx->username ? ctx->username : "salmon";
    
    char endpoint[128];
    snprintf(endpoint, sizeof(endpoint), "/api/mail?type=%s", type);
    char url[512];
    snprintf(url, sizeof(url), "%s%s", ctx->instance_url, endpoint);
    
    CURL *curl = curl_easy_init();
    if (!curl) return -1;
    
    struct curl_slist *headers = create_auth_headers(ctx, endpoint,
                                                     username);
    if (!headers) { curl_easy_cleanup(curl); return -1; }
    
    struct curl_response resp = { .data = malloc(1), .size = 0 };
    resp.data[0] = '\0';
    
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&resp);
    
    CURLcode res = curl_easy_perform(curl);
    int ret = 0;
    
    if (res == CURLE_OK) {
        long http_code;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
        if (http_code == 200) {
            cJSON *json = cJSON_Parse(resp.data);
            if (json) {
                cJSON *mail_array = cJSON_GetObjectItem(json, "mail");
                if (cJSON_IsArray(mail_array)) {
                    int count = cJSON_GetArraySize(mail_array);

                    if (count == 0) {
                        printf("No mail found.\n");
                        cJSON_Delete(json);
                        free(resp.data);
                        curl_slist_free_all(headers);
                        curl_easy_cleanup(curl);
                        return 0;
                    }

                    setup_pager(ctx->plain);

                    char path[512];
                    size_t ksk_len;
                    snprintf(path, sizeof(path), "%s/kem_sk.bin",
                             ctx->config_dir);
                    uint8_t *ksk = load_file(path, &ksk_len);

                    /* build row data first */
                    char **rows_id  = calloc(count, sizeof(char *));
                    char **rows_snd = calloc(count, sizeof(char *));
                    char **rows_sub = calloc(count, sizeof(char *));
                    char **rows_sz  = calloc(count, sizeof(char *));
                    char **rows_dt  = calloc(count, sizeof(char *));
                    int  *rows_szbig = calloc(count, sizeof(int));

                    int is_sent = (strcmp(type, "sent") == 0);
                    for (int i = 0; i < count; i++) {
                        cJSON *item = cJSON_GetArrayItem(mail_array, i);
                        const char *id  = cJSON_GetObjectItem(
                            item, "mail_id")->valuestring;
                        /* sent: show recipient; inbox: show sender */
                        const char *party = is_sent
                            ? cJSON_GetObjectItem(item, "recipient")
                                ->valuestring
                            : cJSON_GetObjectItem(item, "sender")
                                ->valuestring;
                        int sz  = cJSON_GetObjectItem(item, "size")->valueint;
                        int ts  = cJSON_GetObjectItem(item, "timestamp")
                            ->valueint;

                        time_t t = ts;
                        struct tm *tmi = localtime(&t);
                        char date[64];
                        time_t now = time(NULL);
                        const char *fmt = ctx->time_format
                            ? ctx->time_format : "%Y-%m-%d %H:%M";
                        if (ctx->time_format_recent &&
                            (now - t < 180 * 24 * 3600))
                            fmt = ctx->time_format_recent;
                        strftime(date, sizeof(date), fmt, tmi);

                        const char *ekf = is_sent
                            ? "enc_key_sender" : "enc_key_recipient";
                        const char *enc_key = cJSON_GetObjectItem(
                            item, ekf)->valuestring;
                        const char *enc_sub = cJSON_GetObjectItem(
                            item, "enc_subject")->valuestring;

                        char *sub = NULL;
                        if (ksk) {
                            uint8_t *sym_key = kem_decapsulate_key(
                                enc_key, ksk);
                            if (sym_key) {
                                sub = decrypt_from_b64(sym_key, enc_sub);
                                free(sym_key);
                            }
                        }

                        char sz_buf[16];
                        format_size(sz_buf, sizeof(sz_buf), sz);

                        rows_id[i]    = strdup(id);
                        rows_snd[i]   = format_sender(party);
                        rows_sub[i]   = sub ? sub :
                            strdup("(decryption failed)");
                        rows_sz[i]    = strdup(sz_buf);
                        rows_dt[i]    = strdup(date);
                        rows_szbig[i] = (sz >= 1024);
                    }
                    free(ksk);

                    /* compute column widths */
                    int w_id  = 7;  /* "Mail ID" */
                    int w_snd = is_sent ? 9 : 6;
                    int w_sub = 7;  /* "Subject" */
                    int w_sz  = 4;  /* "Size" */
                    int w_dt  = 4;  /* "Date" */

                    for (int i = 0; i < count; i++) {
                        int l;
                        l = (int)strlen(rows_id[i]);
                        if (l > w_id)  w_id = l;
                        l = (int)strlen(rows_snd[i]);
                        if (l > w_snd) w_snd = l;
                        l = (int)strlen(rows_sub[i]);
                        if (l > w_sub) w_sub = l;
                        l = (int)strlen(rows_sz[i]);
                        if (l > w_sz)  w_sz = l;
                        l = (int)strlen(rows_dt[i]);
                        if (l > w_dt)  w_dt = l;
                    }

                    /* cap subject width to terminal */
                    if (!ctx->plain) {
                        int term_width = get_terminal_width();
                        int max_sub = term_width - w_id - w_snd -
                                      w_sz - w_dt - 4;
                        if (max_sub < 15) max_sub = 15;
                        if (w_sub > max_sub) w_sub = max_sub;
                    }

                    /* ANSI helpers */
                    const char *ul_on  = ctx->no_color ? "" : "\033[4m";
                    const char *ul_off = ctx->no_color ? "" : "\033[24m";
                    const char *bold   = ctx->no_color ? "" : "\033[1m";
                    const char *c_w    = ctx->no_color ? "" : "\033[39m";
                    const char *c_cy   = ctx->no_color ? "" : "\033[36m";
                    const char *c_mg   = ctx->no_color ? "" : "\033[35m";
                    const char *c_rs   = ctx->no_color ? "" : "\033[0m";

                    /* print underlined header (per-word underline) */
                    const char *col2_hdr = is_sent ? "Recipient" : "Sender";
                    if (ctx->no_color) {
                        printf("%-*s %-*s %-*s %-*s %s\n",
                               w_id,  "Mail ID",
                               w_snd, col2_hdr,
                               w_sub, "Subject",
                               w_sz,  "Size",
                                      "Date");
                    } else {
                        printf("%s", c_rs);
                        /* Column 0: Mail ID */
                        printf("%s%sMail ID%s%-*s ",
                               c_w, ul_on, ul_off, w_id - 7, "");
                        /* Column 1: Sender / Recipient */
                        printf("%s%s%s%s%-*s ",
                               c_w, ul_on, col2_hdr, ul_off,
                               w_snd - (int)strlen(col2_hdr), "");
                        /* Column 2: Subject */
                        printf("%s%sSubject%s%-*s ",
                               c_w, ul_on, ul_off, w_sub - 7, "");
                        /* Column 3: Size */
                        printf("%s%sSize%s%-*s ",
                               c_w, ul_on, ul_off, w_sz - 4, "");
                        /* Column 4: Date */
                        printf("%s%sDate%s\n",
                               c_w, ul_on, ul_off);
                    }

                    if (count == 0) {
                        printf("No mail found.\n");
                    }

                    for (int i = 0; i < count; i++) {
                        /* truncate subject if needed */
                        char sub_trunc[512];
                        int slen = (int)strlen(rows_sub[i]);
                        if (slen > w_sub && w_sub >= 3) {
                            snprintf(sub_trunc, sizeof(sub_trunc),
                                     "%.*s...", w_sub - 3, rows_sub[i]);
                        } else {
                            snprintf(sub_trunc, sizeof(sub_trunc),
                                     "%s", rows_sub[i]);
                        }

                        const char *c_sz = rows_szbig[i]
                            ? (ctx->no_color ? "" : "\033[1;35m")
                            : c_mg;

                        /* 1. Print Mail ID */
                        printf("%s%-*s%s ", c_rs, w_id, rows_id[i], c_rs);

                        /* 2. Print Sender / Recipient */
                        int len_snd = (int)strlen(rows_snd[i]);
                        if (len_snd > 0 &&
                            rows_snd[i][len_snd - 1] == '@') {
                            printf("%s%.*s", c_cy, len_snd - 1, rows_snd[i]);
                            printf("%s@", c_w);
                        } else {
                            printf("%s%s", c_cy, rows_snd[i]);
                        }
                        printf("%s%-*s ", c_rs, w_snd - len_snd, "");

                        /* 3. Print Subject */
                        printf("%s%-*s%s ", c_w, w_sub, sub_trunc, c_rs);

                        /* 4. Print Size */
                        printf("%s%-*s%s ", c_sz, w_sz, rows_sz[i], c_rs);

                        /* 5. Print Date */
                        printf("%s%s%s\n", c_w, rows_dt[i], c_rs);

                        free(rows_id[i]);
                        free(rows_snd[i]);
                        free(rows_sub[i]);
                        free(rows_sz[i]);
                        free(rows_dt[i]);
                    }
                    free(rows_id); free(rows_snd); free(rows_sub);
                    free(rows_sz);  free(rows_dt); free(rows_szbig);

                    (void)bold; /* reserved for future use */

                    printf("\nTotal: %d item%s\n", count,
                           count == 1 ? "" : "s");
                    wait_pager();
                }
                cJSON_Delete(json);
            }
        } else {
            fprintf(stderr, "Failed to check mail (HTTP %ld): %s\n",
                    http_code, resp.data);
            ret = -1;
        }
    } else {
        fprintf(stderr, "Network error: %s\n",
                curl_easy_strerror(res));
        ret = -1;
    }
    
    free(resp.data);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return ret;
}

int shyake_fetch(shyake_ctx *ctx, const char *mail_id, int raw)
{
    if (!ctx || !mail_id)
        return -1;
    const char *username = ctx->username ? ctx->username : "salmon";
    
    char endpoint[128];
    snprintf(endpoint, sizeof(endpoint), "/api/mail/%s", mail_id);
    char url[512];
    snprintf(url, sizeof(url), "%s%s", ctx->instance_url, endpoint);
    
    CURL *curl = curl_easy_init();
    if (!curl) return -1;
    
    struct curl_slist *headers = create_auth_headers(ctx, endpoint,
                                                     username);
    if (!headers) { curl_easy_cleanup(curl); return -1; }
    
    struct curl_response resp = { .data = malloc(1), .size = 0 };
    resp.data[0] = '\0';
    
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&resp);
    
    CURLcode res = curl_easy_perform(curl);
    int ret = 0;
    
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
                int ts = cJSON_GetObjectItem(json, "timestamp")->valueint;
                
                time_t t = ts;
                struct tm *tm_info = localtime(&t);
                char date[32];
                strftime(date, sizeof(date), "%Y-%m-%d %H:%M", tm_info);
                
                const char *enc_key_field = (strcmp(username, snd) == 0) ?
                    "enc_key_sender" : "enc_key_recipient";
                const char *enc_key = cJSON_GetObjectItem(
                    json, enc_key_field)->valuestring;
                const char *enc_sub = cJSON_GetObjectItem(
                    json, "enc_subject")->valuestring;
                const char *enc_bdy = cJSON_GetObjectItem(
                    json, "enc_body")->valuestring;
                
                char path[512];
                size_t ksk_len;
                snprintf(path, sizeof(path), "%s/kem_sk.bin",
                         ctx->config_dir);
                uint8_t *ksk = load_file(path, &ksk_len);
                
                char *sub = NULL;
                char *bdy = NULL;
                if (ksk) {
                    uint8_t *sym_key = kem_decapsulate_key(enc_key, ksk);
                    if (sym_key) {
                        sub = decrypt_from_b64(sym_key, enc_sub);
                        bdy = decrypt_from_b64(sym_key, enc_bdy);
                        free(sym_key);
                    }
                    free(ksk);
                }
                
                if (raw) {
                    if (bdy) printf("%s", bdy);
                } else {
                    setup_pager(ctx->plain);
                    const char *c_lbl = ctx->no_color ? "" : "\033[1;36m";
                    const char *c_val = ctx->no_color ? "" : "\033[0m";

                    printf("%sFROM:%s  %s\n", c_lbl, c_val, snd);
                    printf("%sTO:%s    %s\n", c_lbl, c_val, rec);
                    printf("%sDATE:%s  %s\n", c_lbl, c_val, date);
                    printf("%sSUBJ:%s  %s\n\n", c_lbl, c_val,
                           sub ? sub : "(decryption failed)");
                    if (bdy) {
                        printf("%s\n", bdy);
                    } else {
                        printf("(body decryption failed)\n");
                    }
                    wait_pager();
                }
                
                free(sub); free(bdy);
                cJSON_Delete(json);
            }
        } else {
            fprintf(stderr, "Failed to fetch mail (HTTP %ld): %s\n",
                    http_code, resp.data);
            ret = -1;
        }
    } else ret = -1;
    
    free(resp.data);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return ret;
}

int shyake_check_one(shyake_ctx *ctx, const char *mail_id)
{
    if (!ctx || !mail_id) return -1;
    const char *username = ctx->username ? ctx->username : "salmon";

    char endpoint[128];
    snprintf(endpoint, sizeof(endpoint), "/api/mail/%s", mail_id);
    char url[512];
    snprintf(url, sizeof(url), "%s%s", ctx->instance_url, endpoint);

    CURL *curl = curl_easy_init();
    if (!curl) return -1;

    struct curl_slist *headers = create_auth_headers(ctx, endpoint, username);
    if (!headers) { curl_easy_cleanup(curl); return -1; }

    struct curl_response resp = { .data = malloc(1), .size = 0 };
    resp.data[0] = '\0';

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&resp);

    CURLcode res = curl_easy_perform(curl);
    int ret = 0;

    if (res == CURLE_OK) {
        long http_code;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
        if (http_code == 200) {
            cJSON *json = cJSON_Parse(resp.data);
            if (json) {
                /* resolve which enc_key to use */
                const char *snd = cJSON_GetObjectItem(
                    json, "sender")->valuestring;
                const char *rec = cJSON_GetObjectItem(
                    json, "recipient")->valuestring;
                int ts = cJSON_GetObjectItem(
                    json, "timestamp")->valueint;
                int sz = cJSON_GetObjectItem(
                    json, "size")->valueint;

                const char *enc_key_field =
                    (strcmp(username, snd) == 0) ?
                    "enc_key_sender" : "enc_key_recipient";
                const char *enc_key = cJSON_GetObjectItem(
                    json, enc_key_field)->valuestring;
                const char *enc_sub = cJSON_GetObjectItem(
                    json, "enc_subject")->valuestring;

                time_t t = ts;
                struct tm *tm_info = localtime(&t);
                char date[32];
                strftime(date, sizeof(date), "%Y-%m-%d %H:%M", tm_info);

                /* decrypt subject */
                char path[512];
                size_t ksk_len;
                snprintf(path, sizeof(path), "%s/kem_sk.bin",
                         ctx->config_dir);
                uint8_t *ksk = load_file(path, &ksk_len);
                char *sub = NULL;
                if (ksk) {
                    uint8_t *sym_key = kem_decapsulate_key(
                        enc_key, ksk);
                    if (sym_key) {
                        sub = decrypt_from_b64(sym_key, enc_sub);
                        free(sym_key);
                    }
                    free(ksk);
                }

                const char *c_lbl = ctx->no_color ? "" : "\033[1;36m";
                const char *c_rs  = ctx->no_color ? "" : "\033[0m";

                /* value column starts at position 6: "FROM: " */
                int tw = get_terminal_width();
                const char *sub_text = sub
                    ? sub : "(decryption failed)";

                printf("%sFROM:%s %s\n", c_lbl, c_rs, snd);
                printf("%sTO:%s   %s\n", c_lbl, c_rs, rec);
                printf("%sSUBJ:%s ", c_lbl, c_rs);
                print_word_wrap(sub_text, 6, tw);
                printf("%sSIZE:%s %d\n", c_lbl, c_rs, sz);
                printf("%sDATE:%s %s\n", c_lbl, c_rs, date);


                free(sub);
                cJSON_Delete(json);
            }
        } else if (http_code == 404) {
            fprintf(stderr, "Mail not found.\n");
            ret = -1;
        } else {
            fprintf(stderr, "Failed (HTTP %ld): %s\n",
                    http_code, resp.data);
            ret = -1;
        }
    } else ret = -1;

    free(resp.data);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return ret;
}

int shyake_burn(shyake_ctx *ctx, const char *mail_id)
{
    if (!ctx || !mail_id) return -1;
    const char *username = ctx->username ? ctx->username : "salmon";

    char endpoint[128];
    snprintf(endpoint, sizeof(endpoint), "/api/mail/%s", mail_id);
    char url[512];
    snprintf(url, sizeof(url), "%s%s", ctx->instance_url, endpoint);

    CURL *curl = curl_easy_init();
    if (!curl) return -1;

    struct curl_slist *headers = create_signed_headers(
        ctx, "DELETE", endpoint, username);
    if (!headers) { curl_easy_cleanup(curl); return -1; }

    struct curl_response resp = { .data = malloc(1), .size = 0 };
    resp.data[0] = '\0';

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "DELETE");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&resp);

    CURLcode res = curl_easy_perform(curl);
    int ret = 0;

    if (res == CURLE_OK) {
        long http_code;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
        if (http_code == 200) {
            printf("Mail burned.\n");
        } else if (http_code == 404) {
            fprintf(stderr, "Mail not found.\n");
            ret = -1;
        } else if (http_code == 403) {
            fprintf(stderr, "Permission denied.\n");
            ret = -1;
        } else {
            fprintf(stderr, "Failed (HTTP %ld): %s\n",
                    http_code, resp.data);
            ret = -1;
        }
    } else ret = -1;

    free(resp.data);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return ret;
}

int shyake_block(shyake_ctx *ctx, const char *target, int unblock)
{
    if (!ctx || !target) return -1;
    const char *username = ctx->username ? ctx->username : "salmon";
    const char *method = unblock ? "DELETE" : "POST";
    const char *endpoint = "/api/block";

    char url[512];
    snprintf(url, sizeof(url), "%s%s", ctx->instance_url, endpoint);

    CURL *curl = curl_easy_init();
    if (!curl) return -1;

    struct curl_slist *headers = create_signed_headers(
        ctx, method, endpoint, username);
    if (!headers) { curl_easy_cleanup(curl); return -1; }
    headers = curl_slist_append(headers, "Content-Type: application/json");

    /* build JSON body */
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
    int ret = 0;

    if (res == CURLE_OK) {
        long http_code;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
        if (http_code == 200 || http_code == 201) {
            if (unblock)
                printf("%s unblocked.\n", target);
            else
                printf("%s blocked.\n", target);
        } else {
            fprintf(stderr, "Failed (HTTP %ld): %s\n",
                    http_code, resp.data);
            ret = -1;
        }
    } else ret = -1;

    free(body_str);
    free(resp.data);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return ret;
}
