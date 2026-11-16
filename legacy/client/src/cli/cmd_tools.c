/* Files and updates: enc, dec, update */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <getopt.h>
#include <ctype.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>
#include "shyake.h"
#include "internal.h"
#include "cli.h"
#include "config.h"
#include "compose.h"
#include "display.h"
#include "drafts.h"
#include "prompt.h"
#include "update.h"

#define SHYAKE_FALLBACK_VERSION_URL \
	"https://shyake.eee.coffee/api/client/version"

/* version endpoint on own instance, fallback when unregistered */
static char *build_version_url(const char *instance)
{
	if (!instance || !*instance)
		return strdup(SHYAKE_FALLBACK_VERSION_URL);
	usize len = strlen(instance);
	while (len > 0 && instance[len - 1] == '/')
		len--;
	const char *path = "/api/client/version";
	char *url = malloc(len + strlen(path) + 1);
	if (!url)
		return NULL;
	memcpy(url, instance, len);
	memcpy(url + len, path, strlen(path) + 1);
	return url;
}

int cmd_enc(int argc, char *argv[], const app_config *cfg,
	    const char *config_dir)
{
	char *recipient = NULL;
	char *out_path = NULL;
	static struct option enc_options[] = {
		{ "to", required_argument, 0, 't' },
		{ "output", required_argument, 0, 'o' },
		{ 0, 0, 0, 0 }
	};
	int opt, opt_idx = 0;
	optind = 2;
	while ((opt = getopt_long(argc, argv, "t:o:", enc_options, &opt_idx)) !=
	       -1) {
		if (opt == 't')
			recipient = optarg;
		else if (opt == 'o')
			out_path = optarg;
	}
	const char *in_path = optind < argc ? argv[optind] : NULL;
	if (!in_path) {
		fprintf(stderr, "Usage: shyake enc <file> [-t <username>] "
				"[-o <output>]\n");
		return EXIT_FAILURE;
	}
	/* instance/user only required when fetching recipient pubkey */
	if (recipient && !need_account(cfg))
		return EXIT_FAILURE;

	shyake_config c = lib_config(cfg, config_dir);
	shyake_ctx *ctx = shyake_init_ctx(&c);
	char *used_path = NULL;
	shyake_err ret =
		shyake_enc_file(ctx, in_path, out_path, recipient, &used_path);
	if (ret == SHYAKE_OK)
		fprintf(stderr, "Encrypted: %s\n", used_path);
	else
		print_lib_error(ctx, "Encryption failed.");
	free(used_path);
	shyake_free_ctx(ctx);
	return ret == SHYAKE_OK ? EXIT_SUCCESS : EXIT_FAILURE;
}

int cmd_dec(int argc, char *argv[], const app_config *cfg,
	    const char *config_dir)
{
	char *out_path = NULL;
	static struct option dec_options[] = {
		{ "output", required_argument, 0, 'o' }, { 0, 0, 0, 0 }
	};
	int opt, opt_idx = 0;
	optind = 2;
	while ((opt = getopt_long(argc, argv, "o:", dec_options, &opt_idx)) !=
	       -1)
		if (opt == 'o')
			out_path = optarg;
	const char *in_path = optind < argc ? argv[optind] : NULL;
	if (!in_path) {
		fprintf(stderr, "Usage: shyake dec <file> [-o <output>]\n");
		return EXIT_FAILURE;
	}

	shyake_config c = lib_config(cfg, config_dir);
	shyake_ctx *ctx = open_ctx(&c);
	if (!ctx)
		return EXIT_FAILURE;
	shyake_err ret;
	if (out_path) {
		ret = shyake_dec_file(ctx, in_path, out_path);
	} else {
		/* without -o the plaintext goes to stdout */
		u8 *pt;
		usize len;
		ret = shyake_dec_file_buf(ctx, in_path, &pt, &len);
		if (ret == SHYAKE_OK) {
			fwrite(pt, 1, len, stdout);
			shyake_wipe(pt, len);
			free(pt);
		}
	}
	if (ret != SHYAKE_OK)
		print_lib_error(ctx, "Decryption failed.");
	else if (out_path)
		fprintf(stderr, "Decrypted: %s\n", out_path);
	shyake_free_ctx(ctx);
	return ret == SHYAKE_OK ? EXIT_SUCCESS : EXIT_FAILURE;
}

/* update without arguments: the versions, and how to update */
static int show_versions(const char *version_url, int debug)
{
	cli_version_info *info = cli_get_latest_version(version_url, debug);
	if (!info) {
		print_failure("Update failed.",
			      "Cannot get the latest version.");
		return EXIT_FAILURE;
	}

	int show_preview = info->pre_release &&
			   cli_version_cmp(info->pre_release, info->release) >
				   0;
	printf("Installed: %s\n", SHYAKE_VERSION);
	printf("Stable:    %s\n", info->release ? info->release : "N/A");
	if (show_preview)
		printf("Preview:   %s\n", info->pre_release);

	int want_stable = !info->release ||
			  strcmp(SHYAKE_VERSION, info->release) != 0;
	int want_preview = show_preview &&
			   strcmp(SHYAKE_VERSION, info->pre_release) != 0;
	if (want_stable || want_preview)
		printf("\n");
	if (want_stable && want_preview)
		printf("Run 'shyake update stable' or "
		       "'shyake update preview' to update.\n");
	else if (want_stable)
		printf("Run 'shyake update stable' to update.\n");
	else if (want_preview)
		printf("Run 'shyake update preview' to install "
		       "the preview release.\n");

	cli_free_version_info(info);
	return EXIT_SUCCESS;
}

int cmd_update(int argc, char *argv[], const app_config *cfg,
	       const char *config_dir)
{
	(void)config_dir;
	char *version_url = build_version_url(cfg->instance);
	if (!version_url)
		return EXIT_FAILURE;

	int ret;
	if (argc < 3)
		ret = show_versions(version_url, cfg->debug);
	else if (strcmp(argv[2], "stable") == 0)
		ret = cli_self_update(version_url, SHYAKE_VERSION,
				      CLI_UPDATE_STABLE, cfg->debug);
	else if (strcmp(argv[2], "preview") == 0)
		ret = cli_self_update(version_url, SHYAKE_VERSION,
				      CLI_UPDATE_PREVIEW, cfg->debug);
	else {
		fprintf(stderr, "Usage: shyake update [stable|preview]\n");
		ret = EXIT_FAILURE;
	}
	free(version_url);
	return ret == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
