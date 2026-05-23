#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <openssl/sha.h>
#include "lib_internal.h"

char*
get_known_host(const char *config_dir, const char *username)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/known_hosts", config_dir);
    FILE *f = fopen(path, "r");
    if (!f) return NULL;

    char line[8192];
    while (fgets(line, sizeof(line), f)) {
        char user[256];
        char fp[256];
        char pk[8192];
        if (sscanf(line, "%255s %255s %8191s", user, fp, pk) == 3) {
            if (strcmp(user, username) == 0) {
                fclose(f);
                return strdup(pk);
            }
        }
    }
    fclose(f);
    return NULL;
}

void
add_known_host(const char *config_dir, const char *username,
               const char *fp, const char *pk_b64)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/known_hosts", config_dir);
    FILE *f = fopen(path, "a");
    if (!f) return;
    fprintf(f, "%s %s %s\n", username, fp, pk_b64);
    fclose(f);
}

void
update_known_host(const char *config_dir, const char *username,
                  const char *fp, const char *pk_b64)
{
    // rewrite known_hosts removing old entry, then append new
    char path[512];
    snprintf(path, sizeof(path), "%s/known_hosts", config_dir);

    char tmp_path[512];
    snprintf(tmp_path, sizeof(tmp_path), "%s/known_hosts.tmp", config_dir);

    FILE *f = fopen(path, "r");
    FILE *ftmp = fopen(tmp_path, "w");
    if (!ftmp) {
        if (f) fclose(f);
        return;
    }

    if (f) {
        char line[8192];
        while (fgets(line, sizeof(line), f)) {
            char user[256];
            char fp_scan[256];
            char pk_scan[8192];
            if (sscanf(line, "%255s %255s %8191s",
                       user, fp_scan, pk_scan) == 3) {
                if (strcmp(user, username) != 0)
                    fprintf(ftmp, "%s %s %s\n", user, fp_scan, pk_scan);
            }
        }
        fclose(f);
    }

    fprintf(ftmp, "%s %s %s\n", username, fp, pk_b64);
    fclose(ftmp);
    rename(tmp_path, path);
}

shyake_fp_result*
shyake_fingerprint(shyake_ctx *ctx, const char *target_user, int do_update)
{
    if (!ctx) return NULL;

    shyake_fp_result *result = calloc(1, sizeof(shyake_fp_result));
    if (!result) return NULL;

    if (!target_user) {
        /* Self fingerprint: compute from local kem_pk.bin */
        char path[512];
        snprintf(path, sizeof(path), "%s/kem_pk.bin", ctx->config_dir);
        size_t pk_len;
        uint8_t *pk = load_file(path, &pk_len);
        if (!pk) {
            free(result);
            return NULL;
        }
        SHA256(pk, pk_len, result->remote_fp);
        free(pk);
        result->has_local = 0;
        result->match = -1;
        return result;
    }

    /* Remote user fingerprint */
    char *recip_pk_b64 = fetch_recipient_pubkey(ctx, target_user);
    if (!recip_pk_b64) {
        free(result);
        return NULL;
    }

    size_t recip_pk_len;
    uint8_t *recip_pk = base64_decode(recip_pk_b64, &recip_pk_len);
    if (!recip_pk) {
        free(recip_pk_b64);
        free(result);
        return NULL;
    }

    SHA256(recip_pk, recip_pk_len, result->remote_fp);
    free(recip_pk);

    char fp_hex[SHA256_DIGEST_LENGTH * 2 + 1];
    for (int i = 0; i < SHA256_DIGEST_LENGTH; i++)
        sprintf(fp_hex + (i * 2), "%02x", result->remote_fp[i]);

    char *local_pk_b64 = get_known_host(ctx->config_dir, target_user);
    if (local_pk_b64) {
        size_t local_pk_len;
        uint8_t *local_pk = base64_decode(local_pk_b64, &local_pk_len);
        if (local_pk) {
            SHA256(local_pk, local_pk_len, result->local_fp);
            free(local_pk);
        }
        free(local_pk_b64);
        result->has_local = 1;
        result->match = (memcmp(result->local_fp, result->remote_fp,
                                SHA256_DIGEST_LENGTH) == 0) ? 1 : 0;
    } else {
        result->has_local = 0;
        result->match = -1;
    }

    if (do_update)
        update_known_host(ctx->config_dir, target_user,
                          fp_hex, recip_pk_b64);

    free(recip_pk_b64);
    return result;
}

void
shyake_free_fp_result(shyake_fp_result *r)
{
    free(r);
}
