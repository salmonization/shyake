#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <pwd.h>
#include <unistd.h>
#include <errno.h>

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

char *get_config_dir(void)
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

    shyake_config cfg = {
        .config_dir = config_dir,
        .instance_url = "https://shyake.eee.coffee"
    };

    shyake_ctx *ctx = shyake_init_ctx(&cfg);
    if (!ctx) {
        fprintf(stderr, "Failed to initialize shyake context.\n");
        free(config_dir);
        return 1;
    }

    if (shyake_generate_keys(ctx) == 0) {
        printf("Keys generated successfully.\n");
    } else {
        fprintf(stderr, "Failed to generate keys.\n");
    }

    shyake_free_ctx(ctx);
    free(config_dir);

    return 0;
}
