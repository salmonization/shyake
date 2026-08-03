#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <pwd.h>
#include <unistd.h>
#include <errno.h>

#include "shyake.h"
#include "prompt.h"

static const char *default_config =
	"# shyake global configuration file\n\n"
	"INSTANCE=\n"
	"USERNAME=\n\n"
	"# Date & Time format (strftime format)\n"
	"# RECENT: less than 180 days old.\n"
	"# ISO 8601 format\n"
	"TIME_FORMAT=\"%Y-%m-%d %H:%M\"\n"
	"# POSIX format\n"
	"# TIME_FORMAT=\"%b %d  %Y\"\n"
	"# TIME_FORMAT_RECENT=\"%b %d %H:%M\"\n\n"
	"# Time zone (default: auto)\n"
	"# Integer offset in hours: 0=UTC, 8=UTC+8, -6=UTC-6\n"
	"TIME_ZONE=auto\n\n"
	"# Display columns for `check` command\n"
	"CHECK_COLUMNS=id,sender,subject,size,date\n\n"
	"# Disable colors (1 = disable)\n"
	"# NO_COLOR=0\n\n"
	"# Editor for 'shyake compose' (default: ed)\n"
	"# Falls back to $VISUAL, then $EDITOR\n"
	"# EDITOR=\"ed\"\n\n"
	"# Default action when running without arguments\n"
	"# 0 = man, 1 = check inbox, 2 = check inbox --count\n"
	"DEFAULT_ACTION=0\n";

char *get_config_dir(void)
{
	const char *homedir;
	if ((homedir = getenv("HOME")) == NULL) {
		homedir = getpwuid(getuid())->pw_dir;
	}

	char *path = malloc(strlen(homedir) + 32);
	if (!path)
		return NULL;
	sprintf(path, "%s/.config/shyake", homedir);
	return path;
}

/* mkdir -p equivalent for a single path */
static int mkdir_p(const char *path)
{
	char tmp[512];
	snprintf(tmp, sizeof(tmp), "%s", path);
	size_t len = strlen(tmp);
	if (len > 0 && tmp[len - 1] == '/')
		tmp[len - 1] = '\0';

	for (char *p = tmp + 1; *p; p++) {
		if (*p == '/') {
			*p = '\0';
			if (mkdir(tmp, 0700) == -1 && errno != EEXIST)
				return -1;
			*p = '/';
		}
	}
	if (mkdir(tmp, 0700) == -1 && errno != EEXIST)
		return -1;
	return 0;
}

int cmd_init(const char *config_dir)
{
	char *allocated = NULL;
	if (!config_dir) {
		allocated = get_config_dir();
		if (!allocated) {
			fprintf(stderr,
				"Failed to determine config directory.\n");
			return 1;
		}
		config_dir = allocated;
	}

	struct stat st = { 0 };
	if (stat(config_dir, &st) == -1) {
		if (mkdir_p(config_dir) == -1) {
			fprintf(stderr, "Failed to create directory %s: %s\n",
				config_dir, strerror(errno));
			free(allocated);
			return 1;
		}
	}

	char config_file[512];
	snprintf(config_file, sizeof(config_file), "%s/config", config_dir);

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
		printf("Config file %s already exists. Skipping.\n",
		       config_file);
	}

	shyake_config cfg = { .config_dir = config_dir,
			      .instance_url = "",
			      .username = "" };

	shyake_ctx *ctx = shyake_init_ctx(&cfg);
	if (!ctx) {
		fprintf(stderr, "Failed to initialize shyake context.\n");
		free(allocated);
		return 1;
	}

	/* Prompt for passphrase only when new keys will actually be generated */
	char kem_pk_path[512], sig_pk_path[512];
	struct stat st2 = { 0 };
	snprintf(kem_pk_path, sizeof(kem_pk_path), "%s/kem_pk.bin", config_dir);
	snprintf(sig_pk_path, sizeof(sig_pk_path), "%s/sig_pk.bin", config_dir);
	if (stat(kem_pk_path, &st2) == -1 || stat(sig_pk_path, &st2) == -1) {
		char kem_sk_path[512];
		snprintf(kem_sk_path, sizeof(kem_sk_path), "%s/kem_sk.bin",
			 config_dir);
		if (!getenv("SHYAKE_PASSPHRASE")) {
			fprintf(stderr, "Key: %s\n", kem_sk_path);
			fflush(stderr);
		}
		char pp1[512], pp2[512];
		memset(pp1, 0, sizeof(pp1));
		memset(pp2, 0, sizeof(pp2));
		if (read_passphrase(
			    "Enter passphrase (empty for no passphrase): ", pp1,
			    sizeof(pp1)) != 0) {
			shyake_free_ctx(ctx);
			free(allocated);
			return 1;
		}
		if (pp1[0] != '\0') {
			if (read_passphrase("Enter same passphrase again: ",
					    pp2, sizeof(pp2)) != 0 ||
			    strcmp(pp1, pp2) != 0) {
				if (strcmp(pp1, pp2) != 0)
					fprintf(stderr,
						"Passphrases do not match.\n");
				memset(pp1, 0, sizeof(pp1));
				memset(pp2, 0, sizeof(pp2));
				shyake_free_ctx(ctx);
				free(allocated);
				return 1;
			}
			memset(pp2, 0, sizeof(pp2));
		}
		shyake_set_passphrase(ctx, pp1);
		memset(pp1, 0, sizeof(pp1));
	}

	if (shyake_generate_keys(ctx) == 0) {
		printf("Keys generated successfully.\n");
	} else {
		fprintf(stderr, "Failed to generate keys.\n");
	}

	shyake_free_ctx(ctx);
	free(allocated);

	return 0;
}
