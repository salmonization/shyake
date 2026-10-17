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
#include "init.h"

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

	usize sz = strlen(homedir) + 32;
	char *path = malloc(sz);
	if (!path)
		return NULL;
	snprintf(path, sz, "%s/.config/shyake", homedir);
	return path;
}

/* mkdir -p equivalent for a single path */
static int mkdir_p(const char *path)
{
	char tmp[512];
	snprintf(tmp, sizeof(tmp), "%s", path);
	usize len = strlen(tmp);
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

/* write the default config unless one exists */
static void write_default_config(const char *config_dir)
{
	char config_file[512];
	snprintf(config_file, sizeof(config_file), "%s/config", config_dir);
	struct stat st = { 0 };
	if (stat(config_file, &st) == 0) {
		printf("Config file %s already exists. Skipping.\n",
		       config_file);
		return;
	}
	FILE *f = fopen(config_file, "w");
	if (!f) {
		fprintf(stderr, "Init failed. Cannot create %s.\n",
			config_file);
		return;
	}
	fputs(default_config, f);
	fclose(f);
	printf("Created default config at %s\n", config_file);
}

/* whether a key pair is still to be generated */
static int keys_missing(const char *config_dir)
{
	char path[512];
	struct stat st = { 0 };
	snprintf(path, sizeof(path), "%s/kem_pk.bin", config_dir);
	if (stat(path, &st) == -1)
		return 1;
	snprintf(path, sizeof(path), "%s/sig_pk.bin", config_dir);
	return stat(path, &st) == -1;
}

/* passphrase for new secret keys, typed twice; -1 on mismatch */
static int ask_key_passphrase(shyake_ctx *ctx, const char *config_dir)
{
	if (!getenv("SHYAKE_PASSPHRASE")) {
		fprintf(stderr, "Key: %s/kem_sk.bin\n", config_dir);
		fflush(stderr);
	}
	char pp1[512], pp2[512];
	shyake_wipe(pp1, sizeof(pp1));
	shyake_wipe(pp2, sizeof(pp2));
	int ret = -1;
	if (read_passphrase("Enter passphrase (empty for no passphrase): ", pp1,
			    sizeof(pp1)) != 0)
		goto out;
	if (pp1[0] != '\0') {
		int rd = read_passphrase("Enter same passphrase again: ", pp2,
					 sizeof(pp2));
		if (strcmp(pp1, pp2) != 0)
			fprintf(stderr, "Passphrases do not match.\n");
		if (rd != 0 || strcmp(pp1, pp2) != 0)
			goto out;
	}
	shyake_set_passphrase(ctx, pp1);
	ret = 0;
out:
	shyake_wipe(pp1, sizeof(pp1));
	shyake_wipe(pp2, sizeof(pp2));
	return ret;
}

int cmd_init(const char *config_dir)
{
	char *allocated = NULL;
	if (!config_dir) {
		allocated = get_config_dir();
		if (!allocated) {
			fprintf(stderr,
				"Init failed. Cannot find the config directory.\n");
			return 1;
		}
		config_dir = allocated;
	}

	struct stat st = { 0 };
	if (stat(config_dir, &st) == -1 && mkdir_p(config_dir) == -1) {
		fprintf(stderr, "Init failed. Cannot create %s (%s).\n",
			config_dir, strerror(errno));
		free(allocated);
		return 1;
	}
	write_default_config(config_dir);

	shyake_config cfg = { .config_dir = config_dir,
			      .instance_url = "",
			      .username = "" };
	shyake_ctx *ctx = shyake_init_ctx(&cfg);
	if (!ctx) {
		fprintf(stderr,
			"Init failed. Cannot create the library context.\n");
		free(allocated);
		return 1;
	}

	/* prompt only when new keys will actually be generated */
	int ret = 0;
	if (keys_missing(config_dir) &&
	    ask_key_passphrase(ctx, config_dir) != 0)
		ret = 1;
	else if (shyake_generate_keys(ctx) == 0)
		printf("Keys generated.\n");
	else
		fprintf(stderr, "Init failed. Cannot generate keys.\n");

	shyake_free_ctx(ctx);
	free(allocated);
	return ret;
}
