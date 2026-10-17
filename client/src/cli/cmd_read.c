/* Reading mail: check, fetch, read, save, burn, and threads */
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

/* saved mail or drafts, shown as a mailbox list */
static void show_local_list(const shyake_saved_list *s, int drafts,
			    const cli_render_opts *ro)
{
	shyake_mail_list ml;
	ml.count = s->count;
	ml.entries = calloc(s->count, sizeof(shyake_mail_entry));
	for (int i = 0; i < s->count; i++) {
		shyake_saved_entry *se = &s->entries[i];
		shyake_mail_entry *me = &ml.entries[i];
		me->mail_id = se->mail_id;
		me->subject = se->subject;
		me->size = se->size;
		me->timestamp = se->timestamp;
		if (drafts) {
			/* party is the recipient */
			me->party = se->recipient[0] ? se->recipient : "(null)";
			me->created = se->created;
			me->is_sent = 1;
		} else {
			/* always show as inbox */
			me->party = se->sender;
			me->is_sent = 0;
		}
	}
	cli_render_mail_list(&ml, ro);
	free(ml.entries);
}

/* check drafts [<id>]: entirely from local disk */
static int check_drafts(int argc, char *argv[], const app_config *cfg,
			const char *config_dir)
{
	shyake_config c = lib_config(cfg, config_dir);
	shyake_ctx *ctx = open_ctx(&c);
	if (!ctx)
		return EXIT_FAILURE;

	int ok = 1;
	if (argc >= 4) {
		/* check drafts <id>: header only, no body */
		shyake_mail_detail *d =
			cli_read_draft(ctx, config_dir, cfg->username, argv[3]);
		ok = d != NULL;
		if (d)
			show_header(ctx, d, cfg);
		else
			print_drafts_error("Draft read failed.");
		shyake_free_mail_detail(d);
	} else {
		/* check drafts: NULL means key failure */
		shyake_saved_list *s =
			cli_list_drafts(ctx, config_dir, cfg->username);
		ok = s != NULL;
		if (!s) {
			print_drafts_error("Listing drafts failed.");
		} else if (s->count == 0) {
			printf("No drafts.\n");
		} else {
			/* drafts use Created + Modified instead of Date */
			static const int cols[] = { COL_ID,	 COL_PARTY,
						    COL_SUBJECT, COL_SIZE,
						    COL_CREATED, COL_MODIFIED };
			cli_render_opts ro = render_opts(cfg);
			ro.id_label = "ID";
			memcpy(ro.col_order, cols, sizeof(cols));
			ro.col_count = 6;
			show_local_list(s, 1, &ro);
		}
		shyake_free_saved_list(s);
	}
	shyake_free_ctx(ctx);
	return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}

/* check saved [<id>]: entirely from local disk */
static int check_saved(int argc, char *argv[], const app_config *cfg,
		       const char *config_dir)
{
	if (!need_username(cfg))
		return EXIT_FAILURE;
	shyake_config c = lib_config(cfg, config_dir);
	shyake_ctx *ctx = open_ctx(&c);
	if (!ctx)
		return EXIT_FAILURE;

	int ok = 1;
	if (argc >= 4) {
		shyake_mail_detail *d = shyake_check_saved_one(ctx, argv[3]);
		ok = d != NULL;
		if (d)
			show_header(ctx, d, cfg);
		else
			print_lib_error(ctx, "Read failed.");
		shyake_free_mail_detail(d);
	} else {
		shyake_saved_list *s = shyake_list_saved(ctx);
		if (s) {
			cli_render_opts ro = render_opts(cfg);
			cli_parse_columns(cfg->check_columns, ro.col_order,
					  &ro.col_count);
			show_local_list(s, 0, &ro);
			shyake_free_saved_list(s);
		} else {
			printf("No saved mail.\n");
		}
	}
	shyake_free_ctx(ctx);
	return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}

/* --count, --json, --csv, --no-header of check inbox|sent */
static void parse_list_options(int argc, char *argv[], cli_render_opts *ro)
{
	static struct option check_options[] = {
		{ "count", no_argument, 0, 'C' },
		{ "json", no_argument, 0, 'J' },
		{ "csv", no_argument, 0, 'S' },
		{ "no-header", no_argument, 0, 'H' },
		{ 0, 0, 0, 0 }
	};
	int opt, opt_idx = 0;
	optind = 3;
	while ((opt = getopt_long(argc, argv, "", check_options, &opt_idx)) !=
	       -1) {
		if (opt == 'C')
			ro->count_only = 1;
		else if (opt == 'J')
			ro->json_out = 1;
		else if (opt == 'S')
			ro->csv_out = 1;
		else if (opt == 'H')
			ro->no_header = 1;
	}
}

int cmd_check(int argc, char *argv[], const app_config *cfg,
	      const char *config_dir)
{
	if (argc < 3) {
		fprintf(stderr,
			"Usage: shyake check <inbox|sent|id> [options]\n");
		return EXIT_FAILURE;
	}
	const char *arg = argv[2];
	if (strcmp(arg, "drafts") == 0)
		return check_drafts(argc, argv, cfg, config_dir);
	if (strcmp(arg, "saved") == 0)
		return check_saved(argc, argv, cfg, config_dir);

	int is_list = strcmp(arg, "inbox") == 0 || strcmp(arg, "sent") == 0;
	cli_render_opts ro = render_opts(cfg);
	if (is_list)
		parse_list_options(argc, argv, &ro);
	shyake_ctx *ctx = account_ctx(cfg, config_dir);
	if (!ctx)
		return EXIT_FAILURE;

	int ok;
	if (is_list) {
		cli_parse_columns(cfg->check_columns, ro.col_order,
				  &ro.col_count);
		shyake_mail_list *list = shyake_check(ctx, arg);
		ok = list != NULL;
		if (list)
			cli_render_mail_list(list, &ro);
		else
			print_lib_error(ctx, "Check failed.");
		shyake_free_mail_list(list);
	} else {
		/* check <id> metadata view */
		shyake_mail_detail *d = shyake_check_one(ctx, arg);
		ok = d != NULL;
		if (d)
			show_header(ctx, d, cfg);
		else
			print_lib_error(ctx, "Check failed.");
		shyake_free_mail_detail(d);
	}
	shyake_free_ctx(ctx);
	return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}

int cmd_fetch(int argc, char *argv[], const app_config *cfg,
	      const char *config_dir)
{
	int raw = parse_raw(argc, argv);
	const char *mail_id = optind < argc ? argv[optind] : NULL;
	if (!mail_id) {
		fprintf(stderr, "A mail ID is required.\n"
				"Usage: shyake fetch <id>\n");
		return EXIT_FAILURE;
	}
	shyake_ctx *ctx = account_ctx(cfg, config_dir);
	if (!ctx)
		return EXIT_FAILURE;
	shyake_mail_detail *d = shyake_fetch(ctx, mail_id);
	if (d)
		show_mail(ctx, d, raw, cfg);
	else
		print_lib_error(ctx, "Fetch failed.");
	int ok = d != NULL;
	shyake_free_mail_detail(d);
	shyake_free_ctx(ctx);
	return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}

/* read drafts <id>: decrypt and display a draft in full */
static int read_draft(const char *draft_id, int raw, const app_config *cfg,
		      const char *config_dir)
{
	if (!draft_id) {
		fprintf(stderr, "Usage: shyake read drafts <id>\n");
		return EXIT_FAILURE;
	}
	shyake_config c = lib_config(cfg, config_dir);
	shyake_ctx *ctx = open_ctx(&c);
	if (!ctx)
		return EXIT_FAILURE;
	shyake_mail_detail *d =
		cli_read_draft(ctx, config_dir, cfg->username, draft_id);
	if (d)
		show_mail(ctx, d, raw, cfg);
	else
		print_drafts_error("Draft read failed.");
	int ok = d != NULL;
	shyake_free_mail_detail(d);
	shyake_free_ctx(ctx);
	return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}

int cmd_read(int argc, char *argv[], const app_config *cfg,
	     const char *config_dir)
{
	int raw = parse_raw(argc, argv);
	const char *mail_id = optind < argc ? argv[optind] : NULL;
	if (mail_id && strcmp(mail_id, "drafts") == 0)
		return read_draft(optind + 1 < argc ? argv[optind + 1] : NULL,
				  raw, cfg, config_dir);
	if (!mail_id) {
		fprintf(stderr, "Usage: shyake read <id>\n");
		return EXIT_FAILURE;
	}
	if (!need_username(cfg))
		return EXIT_FAILURE;

	shyake_config c = lib_config(cfg, config_dir);
	shyake_ctx *ctx = open_ctx(&c);
	if (!ctx)
		return EXIT_FAILURE;
	shyake_mail_detail *d = shyake_read_saved(ctx, mail_id);
	if (d)
		show_mail(ctx, d, raw, cfg);
	else
		print_lib_error(ctx, "Read failed.");
	int ok = d != NULL;
	shyake_free_mail_detail(d);
	shyake_free_ctx(ctx);
	return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}

int cmd_save(int argc, char *argv[], const app_config *cfg,
	     const char *config_dir)
{
	if (argc < 3) {
		fprintf(stderr, "Usage: shyake save <id>\n");
		return EXIT_FAILURE;
	}
	shyake_ctx *ctx = account_ctx(cfg, config_dir);
	if (!ctx)
		return EXIT_FAILURE;
	shyake_err ret = shyake_save_mail(ctx, argv[2]);
	if (ret == SHYAKE_OK)
		printf("Mail saved.\n");
	else
		print_lib_error(ctx, "Save failed.");
	shyake_free_ctx(ctx);
	return ret == SHYAKE_OK ? EXIT_SUCCESS : EXIT_FAILURE;
}

int cmd_burn(int argc, char *argv[], const app_config *cfg,
	     const char *config_dir)
{
	if (argc < 3) {
		fprintf(stderr, "Usage: shyake burn <id>\n");
		return EXIT_FAILURE;
	}
	shyake_ctx *ctx = account_ctx(cfg, config_dir);
	if (!ctx)
		return EXIT_FAILURE;
	shyake_err ret = shyake_burn(ctx, argv[2]);
	if (ret == SHYAKE_OK)
		printf("Mail burned.\n");
	else
		print_lib_error(ctx, "Burn failed.");
	shyake_free_ctx(ctx);
	return ret == SHYAKE_OK ? EXIT_SUCCESS : EXIT_FAILURE;
}

/* thread options; their use depends on the command */
struct thread_opts {
	int flat, json, no_header;
};

/* parse "<cmd> thread <id>" options; NULL id after saying why */
static const char *parse_thread_args(int argc, char *argv[],
				     struct thread_opts *o)
{
	const char *cmd = argv[1];
	int is_check = strcmp(cmd, "check") == 0;
	int is_save = strcmp(cmd, "save") == 0;
	int bad = 0;
	static struct option thread_options[] = {
		{ "flat", no_argument, 0, 'f' },
		{ "json", no_argument, 0, 'J' },
		{ "no-header", no_argument, 0, 'H' },
		{ "raw", no_argument, 0, 'r' },
		{ 0, 0, 0, 0 }
	};
	int opt, opt_idx = 0;
	optind = 3;
	while ((opt = getopt_long(argc, argv, "r", thread_options, &opt_idx)) !=
	       -1) {
		switch (opt) {
		case 'f':
			o->flat = 1;
			bad |= is_check || is_save;
			break;
		case 'J':
			o->json = 1;
			bad |= !is_check;
			break;
		case 'H':
			o->no_header = 1;
			bad |= !is_check;
			break;
		case 'r':
			fprintf(stderr,
				"--raw shows one mail. Use '%s <id> --raw'.\n",
				cmd);
			return NULL;
		default:
			bad = 1;
			break;
		}
	}
	const char *id = optind < argc ? argv[optind] : NULL;
	if (!id || bad) {
		fprintf(stderr, "Usage: shyake %s thread <id>%s\n", cmd,
			is_check ? " [--json] [--no-header]" :
			is_save	 ? "" :
				   " [--flat]");
		return NULL;
	}
	return id;
}

/* save thread: every mail not saved yet */
static int save_thread(shyake_ctx *ctx, const shyake_thread *t)
{
	int ret = EXIT_SUCCESS;
	int saved = 0, had = 0, missing = 0;
	for (int i = 0; i < t->count; i++) {
		const shyake_thread_entry *e = &t->entries[i];
		if (!e->mail_id) {
			missing++;
		} else if (e->where & SHYAKE_IN_SAVED) {
			had++;
		} else if (shyake_save_mail(ctx, e->mail_id) == SHYAKE_OK) {
			saved++;
		} else {
			char what[64];
			snprintf(what, sizeof(what), "Save of %s failed.",
				 e->mail_id);
			print_lib_error(ctx, what);
			ret = EXIT_FAILURE;
		}
	}
	printf("Saved %d mail.", saved);
	if (had)
		printf(" %d %s already saved.", had, had == 1 ? "was" : "were");
	if (missing)
		printf(" %d %s missing.", missing, missing == 1 ? "is" : "are");
	printf("\n");
	return ret;
}

/* fetch or read thread: bodies from the instance, else saved */
static void show_thread_mails(shyake_ctx *ctx, const shyake_thread *t,
			      int local, int flat, const app_config *cfg)
{
	shyake_mail_detail **d = calloc((usize)t->count, sizeof(*d));
	for (int i = 0; d && i < t->count; i++) {
		const shyake_thread_entry *e = &t->entries[i];
		if (!e->mail_id)
			continue;
		if (!local && (e->where & (SHYAKE_IN_INBOX | SHYAKE_IN_SENT)))
			d[i] = shyake_fetch(ctx, e->mail_id);
		if (!d[i] && (e->where & SHYAKE_IN_SAVED))
			d[i] = shyake_read_saved(ctx, e->mail_id);
	}
	cli_render_thread_mails(t, d, flat, cfg->no_color, cfg->plain,
				cfg->tz_hours, cfg->time_format,
				cfg->time_format_recent);
	for (int i = 0; d && i < t->count; i++)
		shyake_free_mail_detail(d[i]);
	free(d);
}

/* check, fetch, save or read thread <id> */
int cmd_thread(int argc, char *argv[], const app_config *cfg,
	       const char *config_dir)
{
	const char *cmd = argv[1];
	int local = strcmp(cmd, "read") == 0;
	struct thread_opts o = { 0 };
	const char *id = parse_thread_args(argc, argv, &o);
	if (!id)
		return EXIT_FAILURE;
	if (!cfg->username || (!local && !cfg->instance)) {
		fprintf(stderr,
			"Missing INSTANCE or USERNAME in config file.\n");
		return EXIT_FAILURE;
	}

	char action[32];
	snprintf(action, sizeof(action), "%c%s failed.", toupper(cmd[0]),
		 cmd + 1);
	shyake_config c = lib_config(cfg, config_dir);
	shyake_ctx *ctx = open_ctx(&c);
	if (!ctx)
		return EXIT_FAILURE;
	shyake_thread *t = shyake_thread_of(ctx, id, local);
	if (!t) {
		print_lib_error(ctx, action);
		shyake_free_ctx(ctx);
		return EXIT_FAILURE;
	}
	if (!t->complete)
		fprintf(stderr, "Your instance does not list replies "
				"(protocol 4). Only saved mail adds "
				"replies.\n");

	int ret = EXIT_SUCCESS;
	if (strcmp(cmd, "check") == 0) {
		cli_render_opts ro = render_opts(cfg);
		ro.json_out = o.json;
		ro.no_header = o.no_header;
		cli_render_thread_list(t, &ro);
	} else if (strcmp(cmd, "save") == 0) {
		ret = save_thread(ctx, t);
	} else {
		show_thread_mails(ctx, t, local, o.flat, cfg);
	}
	shyake_free_thread(t);
	shyake_free_ctx(ctx);
	return ret;
}
