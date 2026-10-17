/* Account commands: whoami, register, rotate, destroy, fingerprint, block */
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

/* remove a file or a directory tree; no shell */
static int remove_tree(const char *path)
{
	struct stat st;
	if (lstat(path, &st) != 0)
		return errno == ENOENT ? 0 : -1;
	if (!S_ISDIR(st.st_mode))
		return unlink(path);
	DIR *d = opendir(path);
	if (!d)
		return -1;
	int ret = 0;
	struct dirent *e;
	while ((e = readdir(d)) != NULL) {
		if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
			continue;
		char sub[1024];
		if (snprintf(sub, sizeof(sub), "%s/%s", path, e->d_name) >=
			    (int)sizeof(sub) ||
		    remove_tree(sub) != 0)
			ret = -1;
	}
	closedir(d);
	return rmdir(path) == 0 ? ret : -1;
}

/* delete only what shyake keeps in the config dir */
static int remove_local_data(const char *config_dir)
{
	static const char *const names[] = {
		"config",	   "kem_pk.bin", "kem_sk.bin",
		"sig_pk.bin",	   "sig_sk.bin", "known_hosts",
		"known_hosts.tmp", "saved",	 "drafts",
	};
	int ret = 0;
	for (usize i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		char path[1024];
		snprintf(path, sizeof(path), "%s/%s", config_dir, names[i]);
		if (remove_tree(path) != 0)
			ret = -1;
	}
	return ret;
}

/* read one line from stdin and compare it with the username */
static int confirm_username(const char *user)
{
	printf("Type your username to confirm: ");
	fflush(stdout);
	char buf[256];
	if (!fgets(buf, sizeof(buf), stdin))
		return 0;
	if (strcmp(cli_trim(buf), user) != 0) {
		fprintf(stderr, "Username mismatch. Aborted.\n");
		return 0;
	}
	return 1;
}

int cmd_whoami(int argc, char *argv[], const app_config *cfg,
	       const char *config_dir)
{
	(void)argc;
	(void)argv;
	if (cfg->username)
		printf("USERNAME: %s\n", cfg->username);
	else
		printf("USERNAME: (not registered)\n");
	printf("INSTANCE: %s\n", cfg->instance ? cfg->instance : "(not set)");
	printf("CONFIG:   %s\n", config_dir);
	return EXIT_SUCCESS;
}

int cmd_register(int argc, char *argv[], const app_config *cfg,
		 const char *config_dir)
{
	char *username = NULL;
	char *instance = NULL;
	static struct option long_options[] = {
		{ "username", required_argument, 0, 'u' },
		{ "instance", required_argument, 0, 'i' },
		{ 0, 0, 0, 0 }
	};
	int opt, option_index = 0;
	optind = 2;
	while ((opt = getopt_long(argc, argv, "u:i:", long_options,
				  &option_index)) != -1) {
		if (opt == 'u')
			username = optarg;
		else if (opt == 'i')
			instance = optarg;
	}

	if (!username) {
		fprintf(stderr, "A username is required.\n"
				"Usage: shyake register -u <username> "
				"-i <url>\n");
		return EXIT_FAILURE;
	}
	const char *inst = instance ? instance : cfg->instance;
	if (!inst) {
		fprintf(stderr, "Missing INSTANCE in config file.\n");
		return EXIT_FAILURE;
	}

	shyake_config c = lib_config(cfg, config_dir);
	c.instance_url = inst;
	c.username = username;
	shyake_ctx *ctx = open_ctx(&c);
	if (!ctx)
		return EXIT_FAILURE;
	fprintf(stderr, "Registering as %s at %s... ", username, inst);
	fflush(stderr);
	shyake_err ret = shyake_register(ctx, username);
	if (ret == SHYAKE_OK) {
		fprintf(stderr, "done.\n");
		printf("Registered.\n");
		cli_set_account(config_dir, username, inst);
	} else if (ret == SHYAKE_ERR_NO_INSTANCE) {
		fprintf(stderr, "\n");
		print_failure("Registration failed.",
			      "No instance is configured.");
	} else {
		fprintf(stderr, "\n");
		print_lib_error(ctx, "Registration failed.");
	}
	shyake_free_ctx(ctx);
	return ret == SHYAKE_OK ? EXIT_SUCCESS : EXIT_FAILURE;
}

int cmd_rotate(int argc, char *argv[], const app_config *cfg,
	       const char *config_dir)
{
	(void)argc;
	(void)argv;
	if (!need_account(cfg))
		return EXIT_FAILURE;
	printf("WARNING: This replaces your key pairs.\n"
	       "  - All mail to and from you on the instance is "
	       "deleted.\n"
	       "  - Your saved mail and drafts stay, but the new keys "
	       "cannot decrypt\n"
	       "    them. Export them first, for example:\n"
	       "      shyake read thread <id> --no-color > mail.txt\n");
	if (!confirm_username(cfg->username))
		return EXIT_FAILURE;

	shyake_config c = lib_config(cfg, config_dir);
	shyake_ctx *ctx = shyake_init_ctx(&c);
	if (prompt_passphrase(ctx, config_dir) != 0 ||
	    prompt_new_passphrase(ctx, config_dir) != 0) {
		shyake_free_ctx(ctx);
		return EXIT_FAILURE;
	}
	fprintf(stderr, "Rotating keys for %s... ", cfg->username);
	fflush(stderr);
	shyake_err ret = shyake_rotate(ctx);
	if (ret == SHYAKE_OK) {
		fprintf(stderr, "done.\n");
		printf("Keys rotated.\n");
	} else {
		fprintf(stderr, "\n");
		print_lib_error(ctx, "Rotation failed.");
	}
	shyake_free_ctx(ctx);
	return ret == SHYAKE_OK ? EXIT_SUCCESS : EXIT_FAILURE;
}

int cmd_destroy(int argc, char *argv[], const app_config *cfg,
		const char *config_dir)
{
	(void)argc;
	(void)argv;
	if (!need_account(cfg))
		return EXIT_FAILURE;
	printf("WARNING: This destroys your account.\n"
	       "  - All mail to and from you on the instance is "
	       "deleted.\n"
	       "  - Your local configuration, keys, saved mail, and "
	       "drafts are deleted.\n"
	       "  - Your username on this instance stays locked and "
	       "cannot be reused.\n");
	if (!confirm_username(cfg->username))
		return EXIT_FAILURE;

	shyake_ctx *ctx = account_ctx(cfg, config_dir);
	if (!ctx)
		return EXIT_FAILURE;
	shyake_err ret = shyake_destroy(ctx);
	if (ret != SHYAKE_OK) {
		print_lib_error(ctx, "Destroy failed.");
	} else {
		printf("Account destroyed.\n");
		if (remove_local_data(config_dir) == 0)
			printf("Local configuration, keys, saved mail, "
			       "and drafts deleted.\n");
		else
			fprintf(stderr,
				"Cannot delete all files in %s. "
				"Delete them yourself.\n",
				config_dir);
	}
	shyake_free_ctx(ctx);
	return ret == SHYAKE_OK ? EXIT_SUCCESS : EXIT_FAILURE;
}

int cmd_fingerprint(int argc, char *argv[], const app_config *cfg,
		    const char *config_dir)
{
	int do_update = 0;
	const char *target_user = NULL;
	for (int i = 2; i < argc; i++) {
		if (strcmp(argv[i], "--update") == 0)
			do_update = 1;
		else if (!target_user)
			target_user = argv[i];
	}
	if (!need_account(cfg))
		return EXIT_FAILURE;

	const char *user = cfg->username;
	int is_self = (target_user == NULL);
	shyake_config c = lib_config(cfg, config_dir);
	shyake_ctx *ctx = shyake_init_ctx(&c);
	if (is_self)
		printf("Fingerprint for %s (local):\n", user);
	else if (!do_update)
		printf("Fetching public key for %s...\n", target_user);

	shyake_fp_result *fp = shyake_fingerprint(ctx, target_user, do_update);
	if (!fp)
		print_lib_error(ctx, "Fingerprint failed.");
	else if (!is_self && do_update)
		/* --update: only confirm, no art */
		printf("Updated known_hosts for %s.\n", target_user);
	else
		cli_render_fingerprint(is_self ? user : target_user, fp,
				       is_self);
	int ok = fp != NULL;
	shyake_free_fp_result(fp);
	shyake_free_ctx(ctx);
	return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}

int cmd_block(int argc, char *argv[], const app_config *cfg,
	      const char *config_dir)
{
	const char *cmd = argv[1];
	int is_unblock = strcmp(cmd, "unblock") == 0;
	if (argc < 3) {
		fprintf(stderr, "Usage: shyake %s <target>\n", cmd);
		return EXIT_FAILURE;
	}
	const char *target = argv[2];
	shyake_ctx *ctx = account_ctx(cfg, config_dir);
	if (!ctx)
		return EXIT_FAILURE;
	shyake_err ret = shyake_block(ctx, target, is_unblock);
	if (ret == SHYAKE_OK)
		printf("%s %s.\n", target,
		       is_unblock ? "unblocked" : "blocked");
	else
		print_lib_error(ctx, is_unblock ? "Unblock failed." :
						  "Block failed.");
	shyake_free_ctx(ctx);
	return ret == SHYAKE_OK ? EXIT_SUCCESS : EXIT_FAILURE;
}

int cmd_blocklist(int argc, char *argv[], const app_config *cfg,
		  const char *config_dir)
{
	(void)argc;
	(void)argv;
	shyake_ctx *ctx = account_ctx(cfg, config_dir);
	if (!ctx)
		return EXIT_FAILURE;
	shyake_block_list *list = shyake_list_blocks(ctx);
	if (!list)
		print_lib_error(ctx, "Listing blocks failed.");
	shyake_free_ctx(ctx);
	if (!list)
		return EXIT_FAILURE;

	if (list->count == 0)
		printf("Block list is empty.\n");
	for (int i = 0; i < list->count; i++) {
		char tbuf[64];
		cli_format_timestamp(list->entries[i].created, cfg->tz_hours,
				     cfg->time_format, NULL, tbuf,
				     sizeof(tbuf));
		printf("%s  %s\n", tbuf, list->entries[i].target);
	}
	shyake_free_block_list(list);
	return EXIT_SUCCESS;
}
