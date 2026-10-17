/* shyake: global flags and command dispatch */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "shyake.h"
#include "internal.h"
#include "cli.h"
#include "config.h"
#include "init.h"
#include "man.h"

/* global flags, taken out of argv before the command runs */
struct flags {
	int plain;
	int debug;
	int no_color;
	const char *config_dir;
};

typedef int (*command_fn)(int argc, char *argv[], const app_config *cfg,
			  const char *config_dir);

static const struct command {
	const char *name;
	command_fn run;
	int threads; /* "<name> thread <id>" runs cmd_thread */
} commands[] = {
	{ "whoami", cmd_whoami, 0 },
	{ "register", cmd_register, 0 },
	{ "send", cmd_send, 0 },
	{ "reply", cmd_reply, 0 },
	{ "compose", cmd_compose, 0 },
	{ "check", cmd_check, 1 },
	{ "fetch", cmd_fetch, 1 },
	{ "burn", cmd_burn, 0 },
	{ "block", cmd_block, 0 },
	{ "unblock", cmd_block, 0 },
	{ "blocklist", cmd_blocklist, 0 },
	{ "rotate", cmd_rotate, 0 },
	{ "fingerprint", cmd_fingerprint, 0 },
	{ "destroy", cmd_destroy, 0 },
	{ "save", cmd_save, 1 },
	{ "read", cmd_read, 1 },
	{ "enc", cmd_enc, 0 },
	{ "encrypt", cmd_enc, 0 },
	{ "dec", cmd_dec, 0 },
	{ "decrypt", cmd_dec, 0 },
	{ "update", cmd_update, 0 },
};

static const struct command *find_command(const char *name)
{
	for (usize i = 0; i < sizeof(commands) / sizeof(*commands); i++)
		if (strcmp(commands[i].name, name) == 0)
			return &commands[i];
	return NULL;
}

/* drop n arguments at index i */
static void drop_args(int *argc, char *argv[], int i, int n)
{
	for (int j = i; j < *argc - n; j++)
		argv[j] = argv[j + n];
	*argc -= n;
}

/* take the global flags out of argv, wherever they are */
static void take_global_flags(int *argc, char *argv[], struct flags *fl)
{
	for (int i = 1; i < *argc; i++) {
		int n = 1;
		if (strcmp(argv[i], "--plain") == 0) {
			fl->plain = 1;
			fl->no_color = 1;
		} else if (strcmp(argv[i], "--debug") == 0) {
			fl->debug = 1;
		} else if (strcmp(argv[i], "--no-color") == 0) {
			fl->no_color = 1;
		} else if ((strcmp(argv[i], "-c") == 0 ||
			    strcmp(argv[i], "--config") == 0) &&
			   i + 1 < *argc) {
			fl->config_dir = argv[i + 1];
			n = 2;
		} else {
			continue;
		}
		drop_args(argc, argv, i, n);
		i--;
	}
	const char *env = getenv("NO_COLOR");
	if (env && env[0])
		fl->no_color = 1;
}

static char *config_dir_path(const struct flags *fl)
{
	return fl->config_dir ? strdup(fl->config_dir) : get_config_dir();
}

/* no command: DEFAULT_ACTION picks check inbox, its count, or man */
static char **default_argv(int *argc, const struct flags *fl)
{
	static char *def_argv_man[] = { "shyake", "man", NULL };
	static char *def_argv_check[] = { "shyake", "check", "inbox", NULL };
	static char *def_argv_count[] = { "shyake", "check", "inbox", "--count",
					  NULL };
	char *config_dir = config_dir_path(fl);
	app_config *cfg = cli_read_config(config_dir);
	int def_act = cfg->default_action;
	cli_free_config(cfg);
	free(config_dir);

	if (def_act == 1) {
		*argc = 3;
		return def_argv_check;
	}
	if (def_act == 2) {
		*argc = 4;
		return def_argv_count;
	}
	*argc = 2;
	return def_argv_man;
}

/* commands that need no config; 1 when cmd is one, *ret its status */
static int run_early(int argc, char *argv[], const struct flags *fl, int *ret)
{
	const char *cmd = argv[1];
	*ret = EXIT_SUCCESS;
	if (strcmp(cmd, "--version") == 0 || strcmp(cmd, "-v") == 0 ||
	    strcmp(cmd, "-V") == 0)
		printf("Use 'shyake version' instead.\n");
	else if (strcmp(cmd, "help") == 0 || strcmp(cmd, "--help") == 0 ||
		 strcmp(cmd, "-h") == 0)
		printf("Use 'shyake man' instead.\n");
	else if (strcmp(cmd, "man") == 0)
		cli_print_man(argc < 3 ? NULL : argv[2]);
	else if (strcmp(cmd, "version") == 0)
		printf("shyake %s\n", SHYAKE_VERSION);
	else if (strcmp(cmd, "init") == 0)
		*ret = cmd_init(fl->config_dir) == 0 ? EXIT_SUCCESS :
						       EXIT_FAILURE;
	else
		return 0;
	return 1;
}

int main(int argc, char *argv[])
{
	struct flags fl = { 0 };
	take_global_flags(&argc, argv, &fl);
	if (argc < 2)
		argv = default_argv(&argc, &fl);

	int ret;
	if (run_early(argc, argv, &fl, &ret))
		return ret;

	const char *cmd = argv[1];
	const struct command *c = find_command(cmd);
	char *config_dir = config_dir_path(&fl);
	app_config *cfg = cli_read_config(config_dir);
	/* the flags override the config file */
	cfg->no_color = cfg->no_color || fl.no_color;
	cfg->plain = fl.plain;
	cfg->debug = fl.debug;
	if (!c) {
		fprintf(stderr, "Unknown command: %s\n", cmd);
		ret = EXIT_FAILURE;
	} else if (c->threads && argc >= 3 && strcmp(argv[2], "thread") == 0) {
		ret = cmd_thread(argc, argv, cfg, config_dir);
	} else {
		ret = c->run(argc, argv, cfg, config_dir);
	}
	cli_free_config(cfg);
	free(config_dir);
	return ret;
}
