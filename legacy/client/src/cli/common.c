/* Shared by the commands: failure messages, config checks, context setup and output */
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

/* print "<action> <reason>"; the reason may be empty */
void print_failure(const char *action, const char *reason)
{
	if (reason && reason[0])
		fprintf(stderr, "%s %s\n", action, reason);
	else
		fprintf(stderr, "%s\n", action);
}

/* action failed in the library, e.g. "Send failed." */
void print_lib_error(shyake_ctx *ctx, const char *action)
{
	print_failure(action, shyake_last_error(ctx));
}

/* action failed in the local drafts store */
void print_drafts_error(const char *action)
{
	print_failure(action, cli_drafts_last_error());
}

/* a send failed; a changed key is fatal and says how to check it */
void print_send_error(shyake_ctx *ctx, shyake_err ret, const char *recipient)
{
	if (ret != SHYAKE_ERR_KEY_MISMATCH) {
		print_lib_error(ctx, "Send failed.");
		return;
	}
	fprintf(stderr,
		"FATAL: %s\nRun 'shyake fingerprint %s' to check the new key.\n",
		shyake_last_error(ctx), recipient);
}

/* the RE: line of a reply: where the mail it answers is now */
char *describe_reply(shyake_ctx *ctx, const shyake_mail_detail *d)
{
	if (!d->reply_ref)
		return NULL;
	int saved = 0;
	char *id = shyake_find_ref(ctx, d->reply_ref, &saved);
	if (!id)
		return strdup("(unavailable)");
	if (!saved)
		return id;
	usize n = strlen(id) + sizeof(" (saved)");
	char *s = malloc(n);
	if (s)
		snprintf(s, n, "%s (saved)", id);
	free(id);
	return s;
}

/* why a subject cannot be sent, or NULL when it can */
const char *subject_problem(const char *subject)
{
	if (!subject || !subject[0])
		return "The subject cannot be empty.";
	if (strlen(subject) > SUBJECT_MAX)
		return "The subject cannot exceed 128 bytes.";
	for (const char *p = subject; *p; p++)
		if (!isspace((unsigned char)*p))
			return NULL;
	return "The subject cannot be only whitespace.";
}

/* a retry cannot help when you are blocked or the recipient is gone */
int retry_may_help(shyake_err ret)
{
	return ret != SHYAKE_ERR_BLOCKED && ret != SHYAKE_ERR_GONE &&
	       ret != SHYAKE_ERR_NOT_FOUND;
}

/* library settings; a value missing from the config is "" */
shyake_config lib_config(const app_config *cfg, const char *config_dir)
{
	shyake_config c = {
		.config_dir = config_dir,
		.instance_url = cfg->instance ? cfg->instance : "",
		.username = cfg->username ? cfg->username : "",
		.plain = cfg->plain,
		.debug = cfg->debug,
		.no_color = cfg->no_color,
	};
	return c;
}

/* a context with the passphrase entered; NULL when it was not */
shyake_ctx *open_ctx(const shyake_config *c)
{
	shyake_ctx *ctx = shyake_init_ctx(c);
	if (prompt_passphrase(ctx, c->config_dir) != 0) {
		shyake_free_ctx(ctx);
		return NULL;
	}
	return ctx;
}

/* the config names an account on an instance; says so when not */
int need_account(const app_config *cfg)
{
	if (cfg->instance && cfg->username)
		return 1;
	fprintf(stderr, "Missing INSTANCE or USERNAME in config file.\n");
	return 0;
}

int need_username(const app_config *cfg)
{
	if (cfg->username)
		return 1;
	fprintf(stderr, "Missing USERNAME in config file.\n");
	return 0;
}

/* the account's context, for a command that talks to the instance */
shyake_ctx *account_ctx(const app_config *cfg, const char *config_dir)
{
	if (!need_account(cfg))
		return NULL;
	shyake_config c = lib_config(cfg, config_dir);
	return open_ctx(&c);
}

/* list rendering options from the config */
cli_render_opts render_opts(const app_config *cfg)
{
	cli_render_opts ro = { 0 };
	ro.no_color = cfg->no_color;
	ro.plain = cfg->plain;
	ro.tz_hours = cfg->tz_hours;
	ro.time_fmt = cfg->time_format;
	ro.time_fmt_recent = cfg->time_format_recent;
	return ro;
}

/* check <id>: one mail without its body */
void show_header(shyake_ctx *ctx, const shyake_mail_detail *d,
		 const app_config *cfg)
{
	char *re = describe_reply(ctx, d);
	cli_render_mail_header(d, re, cfg->no_color, cfg->tz_hours,
			       cfg->time_format, cfg->time_format_recent);
	free(re);
}

/* fetch, read: one mail in full; raw prints the body alone */
void show_mail(shyake_ctx *ctx, const shyake_mail_detail *d, int raw,
	       const app_config *cfg)
{
	char *re = raw ? NULL : describe_reply(ctx, d);
	cli_render_mail_detail(d, re, raw, cfg->no_color, cfg->plain,
			       cfg->tz_hours, cfg->time_format,
			       cfg->time_format_recent);
	free(re);
}

/* --raw, the only option of fetch and read; leaves optind */
int parse_raw(int argc, char *argv[])
{
	static struct option raw_options[] = { { "raw", no_argument, 0, 'r' },
					       { 0, 0, 0, 0 } };
	int raw = 0, opt, opt_idx = 0;
	optind = 2;
	while ((opt = getopt_long(argc, argv, "r", raw_options, &opt_idx)) !=
	       -1)
		if (opt == 'r')
			raw = 1;
	return raw;
}

/* a body from file, or stdin without one; NULL after saying why */
u8 *read_body(const char *file, const char *action, usize *len)
{
	FILE *in = stdin;
	if (file) {
		in = fopen(file, "rb");
		if (!in) {
			fprintf(stderr, "%s failed. Cannot open %s.\n", action,
				file);
			return NULL;
		}
	}
	u8 *body = cli_read_all(in, len);
	if (in != stdin)
		fclose(in);
	if (!body)
		fprintf(stderr, "%s failed. Cannot read the body.\n", action);
	return body;
}
