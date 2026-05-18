#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#include <oqs/oqs.h>

#include "internal.h"

static int save_file(const char *path, const uint8_t *data, size_t len)
{
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    size_t written = fwrite(data, 1, len, f);
    fclose(f);
    return written == len ? 0 : -1;
}

shyake_ctx* shyake_init_ctx(const shyake_config *config)
{
    if (!config || !config->config_dir) {
        return NULL;
    }

    shyake_ctx *ctx = calloc(1, sizeof(shyake_ctx));
    if (!ctx) return NULL;

    ctx->config_dir = strdup(config->config_dir);
    if (config->instance_url) {
        ctx->instance_url = strdup(config->instance_url);
    }

    return ctx;
}

void shyake_free_ctx(shyake_ctx *ctx)
{
    if (!ctx) return;
    free(ctx->config_dir);
    free(ctx->instance_url);
    free(ctx);
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
            uint8_t *public_key = malloc(kem->length_public_key);
            uint8_t *secret_key = malloc(kem->length_secret_key);
            if (OQS_KEM_keypair(kem, public_key, secret_key)
                == OQS_SUCCESS) {
                snprintf(path_sk, sizeof(path_sk), "%s/kem_sk.bin",
                         ctx->config_dir);
                save_file(path_pk, public_key, kem->length_public_key);
                save_file(path_sk, secret_key, kem->length_secret_key);
            } else {
                ret = -1;
            }
            free(public_key);
            free(secret_key);
            OQS_KEM_free(kem);
        } else {
            ret = -1;
        }
    }

    snprintf(path_pk, sizeof(path_pk), "%s/sig_pk.bin", ctx->config_dir);
    if (stat(path_pk, &st) == -1) {
        OQS_SIG *sig = OQS_SIG_new("ML-DSA-65");
        if (sig) {
            uint8_t *public_key = malloc(sig->length_public_key);
            uint8_t *secret_key = malloc(sig->length_secret_key);
            if (OQS_SIG_keypair(sig, public_key, secret_key)
                == OQS_SUCCESS) {
                snprintf(path_sk, sizeof(path_sk), "%s/sig_sk.bin",
                         ctx->config_dir);
                save_file(path_pk, public_key, sig->length_public_key);
                save_file(path_sk, secret_key, sig->length_secret_key);
            } else {
                ret = -1;
            }
            free(public_key);
            free(secret_key);
            OQS_SIG_free(sig);
        } else {
            ret = -1;
        }
    }

    return ret;
}
