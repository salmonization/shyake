#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <pwd.h>
#include <unistd.h>
#include <errno.h>
#include <oqs/oqs.h>

#include "shyake.h"

static const char *default_config =
    "# shyake global configuration file\n\n"
    "INSTANCE=https://shyake.eee.coffee\n\n"
    "# Date & Time format (strftime format)\n"
    "# RECENT: less than 180 days old.\n"
    "# ISO 8601 format\n"
    "TIME_FORMAT=\"%Y-%m-%d %H:%M\"\n"
    "# POSIX format\n"
    "# TIME_FORMAT=\"%b %d  %Y\"\n"
    "# TIME_FORMAT_RECENT=\"%b %d %H:%M\"\n\n"
    "# Time zone (default: UTC)\n"
    "TIME_ZONE=0\n\n"
    "# Display columns for `check` command\n"
    "CHECK_COLUMNS=id,sender,subject,size,date\n\n"
    "# Disable colors (1 = disable)\n"
    "# NO_COLOR=0\n";

static int save_file(const char *path, const uint8_t *data, size_t len)
{
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    size_t written = fwrite(data, 1, len, f);
    fclose(f);
    return written == len ? 0 : -1;
}

static char *get_config_dir(void)
{
    const char *homedir;
    if ((homedir = getenv("HOME")) == NULL) {
        homedir = getpwuid(getuid())->pw_dir;
    }
    
    char *path = malloc(strlen(homedir) + 32);
    if (!path) return NULL;
    sprintf(path, "%s/.config/shyake", homedir);
    return path;
}

int cmd_init(void)
{
    char *config_dir = get_config_dir();
    if (!config_dir) {
        fprintf(stderr, "Failed to determine config directory.\n");
        return 1;
    }

    struct stat st = {0};
    if (stat(config_dir, &st) == -1) {
        // Try creating ~/.config first if it doesn't exist
        char *base_dir = malloc(strlen(config_dir) + 1);
        strcpy(base_dir, config_dir);
        char *last_slash = strrchr(base_dir, '/');
        if (last_slash) {
            *last_slash = '\0';
            if (stat(base_dir, &st) == -1) {
                mkdir(base_dir, 0700);
            }
        }
        free(base_dir);

        if (mkdir(config_dir, 0700) == -1 && errno != EEXIST) {
            fprintf(stderr, "Failed to create directory %s: %s\n", 
                    config_dir, strerror(errno));
            free(config_dir);
            return 1;
        }
    }

    char *config_file = malloc(strlen(config_dir) + 16);
    sprintf(config_file, "%s/config", config_dir);

    if (stat(config_file, &st) == -1) {
        FILE *f = fopen(config_file, "w");
        if (f) {
            fputs(default_config, f);
            fclose(f);
            printf("Created default config at %s\n", config_file);
        } else {
            fprintf(stderr, "Failed to create %s\n", config_file);
        }
    } else {
        printf("Config file %s already exists. Skipping.\n", config_file);
    }

    free(config_file);

    char path_pk[512], path_sk[512];

    sprintf(path_pk, "%s/kem_pk.bin", config_dir);
    if (stat(path_pk, &st) == -1) {
        OQS_KEM *kem = OQS_KEM_new("ML-KEM-768");
        if (kem) {
            uint8_t *public_key = malloc(kem->length_public_key);
            uint8_t *secret_key = malloc(kem->length_secret_key);
            if (OQS_KEM_keypair(kem, public_key, secret_key) ==
                OQS_SUCCESS) {
                sprintf(path_sk, "%s/kem_sk.bin", config_dir);
                save_file(path_pk, public_key, kem->length_public_key);
                save_file(path_sk, secret_key, kem->length_secret_key);
                printf("Generated ML-KEM-768 keypair.\n");
            } else {
                fprintf(stderr, "Failed to generate ML-KEM-768 keypair.\n");
            }
            free(public_key);
            free(secret_key);
            OQS_KEM_free(kem);
        }
    } else {
        printf("KEM keypair already exists. Skipping.\n");
    }

    sprintf(path_pk, "%s/sig_pk.bin", config_dir);
    if (stat(path_pk, &st) == -1) {
        OQS_SIG *sig = OQS_SIG_new("ML-DSA-65");
        if (sig) {
            uint8_t *public_key = malloc(sig->length_public_key);
            uint8_t *secret_key = malloc(sig->length_secret_key);
            if (OQS_SIG_keypair(sig, public_key, secret_key) ==
                OQS_SUCCESS) {
                sprintf(path_sk, "%s/sig_sk.bin", config_dir);
                save_file(path_pk, public_key, sig->length_public_key);
                save_file(path_sk, secret_key, sig->length_secret_key);
                printf("Generated ML-DSA-65 keypair.\n");
            } else {
                fprintf(stderr, "Failed to generate ML-DSA-65 keypair.\n");
            }
            free(public_key);
            free(secret_key);
            OQS_SIG_free(sig);
        }
    } else {
        printf("SIG keypair already exists. Skipping.\n");
    }

    free(config_dir);

    return 0;
}
