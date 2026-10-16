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
#include "display.h"
#include "prompt.h"
#include "update.h"
#include "drafts.h"
#include "man.h"
#include "config.h"
#include "compose.h"
#include "init.h"
#include "internal.h"

#ifndef SHYAKE_VERSION
#define SHYAKE_VERSION "dev"
#endif
#define SHYAKE_FALLBACK_VERSION_URL \
	"https://shyake.eee.coffee/api/client/version"

/* global flags, taken out of argv before the command runs */
static int global_plain = 0;
static int global_debug = 0;
static int global_no_color = 0;
static const char *global_config_dir = NULL;

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

/* print "<action> <reason>"; the reason may be empty */
static void print_failure(const char *action, const char *reason)
{
	if (reason && reason[0])
		fprintf(stderr, "%s %s\n", action, reason);
	else
		fprintf(stderr, "%s\n", action);
}

/* action failed in the library, e.g. "Send failed." */
static void print_lib_error(shyake_ctx *ctx, const char *action)
{
	print_failure(action, shyake_last_error(ctx));
}

/* action failed in the local drafts store */
static void print_drafts_error(const char *action)
{
	print_failure(action, cli_drafts_last_error());
}

/* a send failed; a changed key is fatal and says how to check it */
static void print_send_error(shyake_ctx *ctx, shyake_err ret,
			     const char *recipient)
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
static char *describe_reply(shyake_ctx *ctx, const shyake_mail_detail *d)
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

/* keep the text of a send that failed, so it is not lost */
static void rescue_draft(shyake_ctx *ctx, const char *config_dir,
			 const char *recipient, const char *subject,
			 const char *reply_ref, const u8 *body, usize body_len,
			 int retry)
{
	char *new_id = NULL;
	if (cli_save_draft(ctx, config_dir, recipient, subject, reply_ref, body,
			   body_len, NULL, &new_id) != SHYAKE_OK) {
		print_drafts_error("Draft save failed.");
		return;
	}
	if (new_id && retry) {
		printf("Saved as draft %s. Retry with: shyake send -d %s\n",
		       new_id, new_id);
		free(new_id);
	} else if (new_id) {
		printf("Saved as draft %s.\n", new_id);
		free(new_id);
	} else {
		printf("Saved as a draft.\n");
	}
}

/* why a subject cannot be sent, or NULL when it can */
static const char *subject_problem(const char *subject)
{
	if (!subject || !subject[0])
		return "The subject cannot be empty.";
	if (strlen(subject) > 128)
		return "The subject cannot exceed 128 bytes.";
	for (const char *p = subject; *p; p++)
		if (!isspace((unsigned char)*p))
			return NULL;
	return "The subject cannot be only whitespace.";
}

/* "Re: <subject>", kept within 128 bytes on a UTF-8 boundary */
static char *reply_subject(const char *orig)
{
	const char *prefix = strncasecmp(orig, "Re:", 3) == 0 ? "" : "Re: ";
	usize n = strlen(prefix) + strlen(orig);
	if (n > 128) {
		n = 128;
		while (n > strlen(prefix) &&
		       ((u8)orig[n - strlen(prefix)] & 0xC0) == 0x80)
			n--;
	}
	char *s = malloc(n + 1);
	if (s)
		snprintf(s, n + 1, "%s%s", prefix, orig);
	return s;
}

/* a retry cannot help when you are blocked or the recipient is gone */
static int retry_may_help(shyake_err ret)
{
	return ret != SHYAKE_ERR_BLOCKED && ret != SHYAKE_ERR_GONE &&
	       ret != SHYAKE_ERR_NOT_FOUND;
}

/* ------------------------------------------------------------------ */
/* Command setup                                                      */
/* ------------------------------------------------------------------ */

/* --no-color, --plain, NO_COLOR or the config */
static int no_color(const app_config *cfg)
{
	return global_no_color || cfg->no_color;
}

/* library settings; a value missing from the config is "" */
static shyake_config lib_config(const app_config *cfg, const char *config_dir)
{
	shyake_config c = {
		.config_dir = config_dir,
		.instance_url = cfg->instance ? cfg->instance : "",
		.username = cfg->username ? cfg->username : "",
		.plain = global_plain,
		.debug = global_debug,
		.no_color = no_color(cfg),
	};
	return c;
}

/* a context with the passphrase entered; NULL when it was not */
static shyake_ctx *open_ctx(const shyake_config *c)
{
	shyake_ctx *ctx = shyake_init_ctx(c);
	if (prompt_passphrase(ctx, c->config_dir) != 0) {
		shyake_free_ctx(ctx);
		return NULL;
	}
	return ctx;
}

/* the config names an account on an instance; says so when not */
static int need_account(const app_config *cfg)
{
	if (cfg->instance && cfg->username)
		return 1;
	fprintf(stderr, "Missing INSTANCE or USERNAME in config file.\n");
	return 0;
}

static int need_username(const app_config *cfg)
{
	if (cfg->username)
		return 1;
	fprintf(stderr, "Missing USERNAME in config file.\n");
	return 0;
}

/* the account's context, for a command that talks to the instance */
static shyake_ctx *account_ctx(const app_config *cfg, const char *config_dir)
{
	if (!need_account(cfg))
		return NULL;
	shyake_config c = lib_config(cfg, config_dir);
	return open_ctx(&c);
}

/* list rendering options from the config */
static cli_render_opts render_opts(const app_config *cfg)
{
	cli_render_opts ro = { 0 };
	ro.no_color = no_color(cfg);
	ro.plain = global_plain;
	ro.tz_hours = cfg->tz_hours;
	ro.time_fmt = cfg->time_format;
	ro.time_fmt_recent = cfg->time_format_recent;
	return ro;
}

/* check <id>: one mail without its body */
static void show_header(shyake_ctx *ctx, const shyake_mail_detail *d,
			const app_config *cfg)
{
	char *re = describe_reply(ctx, d);
	cli_render_mail_header(d, re, no_color(cfg), cfg->tz_hours,
			       cfg->time_format, cfg->time_format_recent);
	free(re);
}

/* fetch, read: one mail in full; raw prints the body alone */
static void show_mail(shyake_ctx *ctx, const shyake_mail_detail *d, int raw,
		      const app_config *cfg)
{
	char *re = raw ? NULL : describe_reply(ctx, d);
	cli_render_mail_detail(d, re, raw, no_color(cfg), global_plain,
			       cfg->tz_hours, cfg->time_format,
			       cfg->time_format_recent);
	free(re);
}

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

/* --raw, the only option of fetch and read; leaves optind */
static int parse_raw(int argc, char *argv[])
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
static u8 *read_body(const char *file, const char *action, usize *len)
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

/*
 * Parse an edited template and store it as a draft. A reply's
 * draft keeps reply_to as its recipient, whatever the template says.
 * out_id, when not NULL, receives the draft's id. Returns an exit
 * status.
 */
static int save_edited_draft(shyake_ctx *ctx, const char *config_dir, char *buf,
			     const char *reply_ref, const char *reply_to,
			     const char *draft_id, char **out_id)
{
	char *to, *subject, *body;
	if (cli_parse_compose(buf, &to, &subject, &body) != 0) {
		fprintf(stderr, "Missing '---' separator. Draft aborted.\n");
		return EXIT_FAILURE;
	}
	while (*body == '\n' || *body == '\r')
		body++;
	int has_content = 0;
	for (const char *p = body; *p && !has_content; p++)
		has_content = !isspace((unsigned char)*p);
	if (!has_content) {
		fprintf(stderr, "Draft aborted (empty body).\n");
		return EXIT_FAILURE;
	}

	if (subject && strlen(subject) > 128)
		fprintf(stderr, "WARNING: The subject exceeds 128 bytes. "
				"Sending will fail.\n");
	if (reply_to) {
		if (to && to[0] && strcmp(to, reply_to) != 0)
			fprintf(stderr, "A reply keeps its recipient: %s.\n",
				reply_to);
		to = (char *)reply_to;
	}

	char *new_id = NULL;
	shyake_err ret = cli_save_draft(ctx, config_dir, to, subject, reply_ref,
					(const u8 *)body, strlen(body),
					draft_id, &new_id);
	const char *id = new_id ? new_id : draft_id;
	if (ret != SHYAKE_OK)
		print_drafts_error("Draft save failed.");
	else
		printf("Draft %s saved.\n", id);
	if (ret == SHYAKE_OK && out_id)
		*out_id = strdup(id);
	free(new_id);
	return ret == SHYAKE_OK ? EXIT_SUCCESS : EXIT_FAILURE;
}

/*
 * A reply's draft is saved; send it only when the user says so.
 * Sent, the draft is deleted; otherwise it stays. Returns an exit
 * status.
 */
static int offer_reply_send(shyake_ctx *ctx, const char *config_dir,
			    const char *username, const char *draft_id,
			    const char *recipient)
{
	char question[320];
	snprintf(question, sizeof(question), "Send it to %s now? [y/N] ",
		 recipient);
	if (!ask_yes(question)) {
		printf("Not sent. Send it with: shyake send -d %s\n", draft_id);
		return EXIT_SUCCESS;
	}

	shyake_mail_detail *d =
		cli_read_draft(ctx, config_dir, username, draft_id);
	if (!d || !d->body || !d->reply_ref) {
		print_drafts_error("Draft read failed.");
		shyake_free_mail_detail(d);
		return EXIT_FAILURE;
	}
	const char *problem = subject_problem(d->subject);
	if (problem) {
		fprintf(stderr, "%s Edit it with: shyake compose %s\n", problem,
			draft_id);
		shyake_free_mail_detail(d);
		return EXIT_FAILURE;
	}
	fprintf(stderr, "Replying to %s... ", recipient);
	fflush(stderr);
	shyake_err ret = shyake_send_reply(ctx, recipient, d->reply_ref,
					   d->subject, (const u8 *)d->body,
					   strlen(d->body));
	if (ret == SHYAKE_OK) {
		fprintf(stderr, "done.\n");
		printf("Mail sent.\n");
		if (cli_delete_draft(config_dir, draft_id) == SHYAKE_OK)
			printf("Draft %s deleted.\n", draft_id);
	} else {
		fprintf(stderr, "\n");
		print_send_error(ctx, ret, recipient);
		if (retry_may_help(ret))
			printf("Draft %s is kept. Retry with: shyake send -d "
			       "%s\n",
			       draft_id, draft_id);
		else
			printf("Draft %s is kept.\n", draft_id);
	}
	shyake_free_mail_detail(d);
	return ret == SHYAKE_OK ? EXIT_SUCCESS : EXIT_FAILURE;
}

/* ------------------------------------------------------------------ */
/* Account commands                                                   */
/* ------------------------------------------------------------------ */

static int cmd_whoami(int argc, char *argv[], const app_config *cfg,
		      const char *config_dir)
{
	(void)argc;
	(void)argv;
	if (cfg->username)
		printf("USERNAME: %s\n", cfg->username);
	else
		printf("USERNAME: (not registered)\n");
	printf("INSTANCE: %s\n", cfg->instance);
	printf("CONFIG:   %s\n", config_dir);
	return EXIT_SUCCESS;
}

static int cmd_register(int argc, char *argv[], const app_config *cfg,
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

static int cmd_rotate(int argc, char *argv[], const app_config *cfg,
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

static int cmd_destroy(int argc, char *argv[], const app_config *cfg,
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

static int cmd_fingerprint(int argc, char *argv[], const app_config *cfg,
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

static int cmd_block(int argc, char *argv[], const app_config *cfg,
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

static int cmd_blocklist(int argc, char *argv[], const app_config *cfg,
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

/* ------------------------------------------------------------------ */
/* Sending                                                            */
/* ------------------------------------------------------------------ */

/* the body's first line becomes the subject; the rest stays */
static char *take_first_line(u8 *body, usize *len)
{
	u8 *newline = memchr(body, '\n', *len);
	usize line_len = newline ? (usize)(newline - body) : *len;
	usize sub_len = line_len;
	if (sub_len > 0 && body[sub_len - 1] == '\r')
		sub_len--;

	char *subject = malloc(sub_len + 1);
	memcpy(subject, body, sub_len);
	subject[sub_len] = '\0';

	usize advance = newline ? line_len + 1 : *len;
	*len -= advance;
	if (*len > 0)
		memmove(body, body + advance, *len);
	return subject;
}

/* a draft read back, sent; -t and -s fill what it lacks */
static int send_loaded_draft(shyake_ctx *ctx, const char *config_dir,
			     const char *draft_id, const shyake_mail_detail *d,
			     const char *recipient, const char *subject)
{
	if (!recipient && d->recipient && d->recipient[0])
		recipient = d->recipient;
	if (!subject && d->subject && d->subject[0])
		subject = d->subject;
	if (!recipient) {
		fprintf(stderr, "The draft has no recipient. "
				"Use -t <username>.\n");
		return EXIT_FAILURE;
	}
	if (!subject) {
		fprintf(stderr, "The draft has no subject. "
				"Use -s <subject>.\n");
		return EXIT_FAILURE;
	}
	if (strlen(subject) > 128) {
		fprintf(stderr, "The subject cannot exceed 128 bytes.\n");
		return EXIT_FAILURE;
	}

	fprintf(stderr, "Sending draft %s to %s... ", draft_id, recipient);
	fflush(stderr);
	const u8 *body = (const u8 *)d->body;
	shyake_err ret = d->reply_ref ?
				 shyake_send_reply(ctx, recipient, d->reply_ref,
						   subject, body,
						   strlen(d->body)) :
				 shyake_send(ctx, recipient, subject, body,
					     strlen(d->body));
	if (ret != SHYAKE_OK) {
		fprintf(stderr, "\n");
		print_send_error(ctx, ret, recipient);
		return EXIT_FAILURE;
	}
	fprintf(stderr, "done.\n");
	printf("Mail sent.\n");
	if (cli_delete_draft(config_dir, draft_id) == SHYAKE_OK)
		printf("Draft %s deleted.\n", draft_id);
	return EXIT_SUCCESS;
}

/* send -d: send a stored draft, delete it on success */
static int send_draft(const app_config *cfg, const char *config_dir,
		      const char *draft_id, const char *recipient,
		      const char *subject)
{
	shyake_ctx *ctx = account_ctx(cfg, config_dir);
	if (!ctx)
		return EXIT_FAILURE;

	int status = EXIT_FAILURE;
	shyake_mail_detail *d =
		cli_read_draft(ctx, config_dir, cfg->username, draft_id);
	if (!d || !d->body)
		print_drafts_error("Draft read failed.");
	else if (d->reply_ref && recipient && d->recipient &&
		 strcmp(recipient, d->recipient) != 0)
		/* a reply goes where its draft says */
		fprintf(stderr,
			"Draft %s is a reply to %s. "
			"-t cannot change that.\n",
			draft_id, d->recipient);
	else
		status = send_loaded_draft(ctx, config_dir, draft_id, d,
					   recipient, subject);
	shyake_free_mail_detail(d);
	shyake_free_ctx(ctx);
	return status;
}

static int cmd_send(int argc, char *argv[], const app_config *cfg,
		    const char *config_dir)
{
	char *recipient = NULL;
	char *subject = NULL;
	char *draft_id = NULL;
	static struct option long_options[] = {
		{ "to", required_argument, 0, 't' },
		{ "subject", required_argument, 0, 's' },
		{ "draft", required_argument, 0, 'd' },
		{ 0, 0, 0, 0 }
	};
	int opt, option_index = 0;
	optind = 2;
	while ((opt = getopt_long(argc, argv, "t:s:d:", long_options,
				  &option_index)) != -1) {
		if (opt == 't')
			recipient = optarg;
		else if (opt == 's')
			subject = optarg;
		else if (opt == 'd')
			draft_id = optarg;
	}

	if (draft_id)
		return send_draft(cfg, config_dir, draft_id, recipient,
				  subject);
	if (!recipient) {
		fprintf(stderr, "A recipient is required.\n"
				"Usage: shyake send -t <username> "
				"[-s <subject>] [<file>]\n");
		return EXIT_FAILURE;
	}

	usize body_len;
	u8 *body = read_body(optind < argc ? argv[optind] : NULL, "Send",
			     &body_len);
	if (!body)
		return EXIT_FAILURE;
	char *first_line = subject ? NULL : take_first_line(body, &body_len);
	if (first_line)
		subject = first_line;

	int status = EXIT_FAILURE;
	const char *problem = subject_problem(subject);
	shyake_ctx *ctx = NULL;
	if (problem)
		fprintf(stderr, "%s\n", problem);
	else
		ctx = account_ctx(cfg, config_dir);
	if (ctx) {
		fprintf(stderr, "Sending mail to %s... ", recipient);
		fflush(stderr);
		shyake_err ret =
			shyake_send(ctx, recipient, subject, body, body_len);
		if (ret == SHYAKE_OK) {
			fprintf(stderr, "done.\n");
			printf("Mail sent.\n");
			status = EXIT_SUCCESS;
		} else {
			fprintf(stderr, "\n");
			print_send_error(ctx, ret, recipient);
			/* a failed send must not eat what the user wrote */
			rescue_draft(ctx, config_dir, recipient, subject, NULL,
				     body, body_len, retry_may_help(ret));
		}
		shyake_free_ctx(ctx);
	}
	free(body);
	free(first_line);
	return status;
}

/* ------------------------------------------------------------------ */
/* Replies and drafts                                                 */
/* ------------------------------------------------------------------ */

/* a reply being written to one mail */
struct reply {
	shyake_ctx *ctx;
	const app_config *cfg;
	const char *config_dir;
	const char *mail_id; /* the mail answered */
	const shyake_mail_detail *orig;
	const char *recipient;
	const char *subject; /* -s, else "Re: ..."; NULL if unknown */
};

/* reply -d: turn a stored draft into this reply */
static int reply_with_draft(const struct reply *r, const char *draft_id)
{
	shyake_mail_detail *d = cli_read_draft(r->ctx, r->config_dir,
					       r->cfg->username, draft_id);
	if (!d || !d->body) {
		print_drafts_error("Draft read failed.");
		shyake_free_mail_detail(d);
		return EXIT_FAILURE;
	}
	if (d->recipient && d->recipient[0] &&
	    strcmp(d->recipient, r->recipient) != 0)
		fprintf(stderr, "Draft %s now goes to %s.\n", draft_id,
			r->recipient);
	const char *sub = d->subject && d->subject[0] &&
					  !subject_problem(d->subject) ?
				  d->subject :
				  r->subject;

	int status = EXIT_FAILURE;
	if (cli_save_draft(r->ctx, r->config_dir, r->recipient, sub,
			   r->orig->ref, (const u8 *)d->body, strlen(d->body),
			   draft_id, NULL) == SHYAKE_OK) {
		printf("Draft %s is now a reply to %s.\n", draft_id,
		       r->mail_id);
		status = offer_reply_send(r->ctx, r->config_dir,
					  r->cfg->username, draft_id,
					  r->recipient);
	} else {
		print_drafts_error("Draft save failed.");
	}
	shyake_free_mail_detail(d);
	return status;
}

/* legacy: a body from a file or a pipe is sent at once */
static int reply_now(const struct reply *r, const u8 *body, usize body_len)
{
	if (!r->subject) {
		fprintf(stderr,
			"Reply failed. The subject of mail %s "
			"cannot be decrypted. Use -s <subject>.\n",
			r->mail_id);
		return EXIT_FAILURE;
	}
	fprintf(stderr, "Replying to %s... ", r->recipient);
	fflush(stderr);
	shyake_err ret = shyake_send_reply(r->ctx, r->recipient, r->orig->ref,
					   r->subject, body, body_len);
	if (ret == SHYAKE_OK) {
		fprintf(stderr, "done.\n");
		printf("Mail sent.\n");
		return EXIT_SUCCESS;
	}
	fprintf(stderr, "\n");
	print_send_error(r->ctx, ret, r->recipient);
	rescue_draft(r->ctx, r->config_dir, r->recipient, r->subject,
		     r->orig->ref, body, body_len, retry_may_help(ret));
	return EXIT_FAILURE;
}

/* write the reply in the editor, beside the mail */
static int reply_in_editor(const struct reply *r)
{
	const char *sub = r->subject ? r->subject : "";
	usize n = strlen(sub) + 32;
	char *initial = malloc(n);
	char *shown = cli_format_original(r->orig, r->cfg);
	char *buf = NULL;
	if (initial && shown) {
		snprintf(initial, n, "Subject: %s\n---\n\n", sub);
		buf = cli_edit_draft(r->cfg, r->config_dir, initial, shown);
	}

	int status = EXIT_FAILURE;
	char *new_id = NULL;
	if (buf)
		status = save_edited_draft(r->ctx, r->config_dir, buf,
					   r->orig->ref, r->recipient, NULL,
					   &new_id);
	if (new_id)
		status = offer_reply_send(r->ctx, r->config_dir,
					  r->cfg->username, new_id,
					  r->recipient);
	free(new_id);
	free(buf);
	free(initial);
	free(shown);
	return status;
}

static int cmd_reply(int argc, char *argv[], const app_config *cfg,
		     const char *config_dir)
{
	char *subject = NULL;
	const char *draft_id = NULL;
	static struct option long_options[] = {
		{ "subject", required_argument, 0, 's' },
		{ "draft", required_argument, 0, 'd' },
		{ 0, 0, 0, 0 }
	};
	int opt, option_index = 0;
	optind = 2;
	while ((opt = getopt_long(argc, argv, "s:d:", long_options,
				  &option_index)) != -1) {
		if (opt == 's')
			subject = optarg;
		else if (opt == 'd')
			draft_id = optarg;
	}

	if (optind >= argc) {
		fprintf(stderr, "A mail ID is required.\n"
				"Usage: shyake reply <id> "
				"[-s <subject>] [-d <draft>]\n");
		return EXIT_FAILURE;
	}
	const char *mail_id = argv[optind++];
	const char *problem = subject ? subject_problem(subject) : NULL;
	if (problem) {
		fprintf(stderr, "%s\n", problem);
		return EXIT_FAILURE;
	}
	if (!need_account(cfg))
		return EXIT_FAILURE;

	int direct = !draft_id && (optind < argc || !isatty(STDIN_FILENO));
	usize body_len = 0;
	u8 *body = NULL;
	if (direct) {
		body = read_body(optind < argc ? argv[optind] : NULL, "Reply",
				 &body_len);
		if (!body)
			return EXIT_FAILURE;
	}
	shyake_config c = lib_config(cfg, config_dir);
	shyake_ctx *ctx = open_ctx(&c);
	if (!ctx) {
		free(body);
		return EXIT_FAILURE;
	}

	/* the mail answered gives recipient, reference, subject */
	int status = EXIT_FAILURE;
	shyake_mail_detail *orig = shyake_fetch(ctx, mail_id);
	const char *recipient = orig ? shyake_reply_recipient(ctx, orig) : NULL;
	if (!orig) {
		print_lib_error(ctx, "Reply failed.");
	} else if (!orig->ref || !recipient) {
		fprintf(stderr, "Reply failed. Mail %s cannot be answered.\n",
			mail_id);
	} else {
		char *re_subject =
			orig->subject ? reply_subject(orig->subject) : NULL;
		struct reply r = { ctx,
				   cfg,
				   config_dir,
				   mail_id,
				   orig,
				   recipient,
				   subject ? subject : re_subject };
		if (draft_id)
			status = reply_with_draft(&r, draft_id);
		else if (direct)
			status = reply_now(&r, body, body_len);
		else
			status = reply_in_editor(&r);
		free(re_subject);
	}
	shyake_free_mail_detail(orig);
	free(body);
	shyake_free_ctx(ctx);
	return status;
}

/* a compose template, and what a reply's draft keeps */
struct template
{
	char *initial;
	char *shown; /* the mail a reply answers */
	char *reply_ref;
	char *reply_to;
};

static void free_template(struct template *t)
{
	free(t->initial);
	free(t->shown);
	free(t->reply_ref);
	free(t->reply_to);
}

/* the mail a reference names, formatted for the editor */
static char *original_of(shyake_ctx *ctx, const app_config *cfg,
			 const char *ref)
{
	int saved = 0;
	char *oid = shyake_find_ref(ctx, ref, &saved);
	shyake_mail_detail *o = !oid  ? NULL :
				saved ? shyake_read_saved(ctx, oid) :
					shyake_fetch(ctx, oid);
	char *shown = o ? cli_format_original(o, cfg) : NULL;
	shyake_free_mail_detail(o);
	free(oid);
	return shown;
}

/* the template of a stored draft; -1 after saying why not */
static int draft_template(shyake_ctx *ctx, const app_config *cfg,
			  const char *config_dir, const char *draft_id,
			  struct template *t)
{
	shyake_mail_detail *d =
		cli_read_draft(ctx, config_dir, cfg->username, draft_id);
	if (!d || !d->body) {
		print_drafts_error("Draft read failed.");
		shyake_free_mail_detail(d);
		return -1;
	}
	const char *to = d->recipient ? d->recipient : "";
	const char *sub = d->subject ? d->subject : "";
	usize sz = strlen(to) + strlen(sub) + strlen(d->body) + 32;
	t->initial = malloc(sz);
	if (d->reply_ref) {
		/* a reply: fixed recipient, the mail beside */
		t->reply_ref = strdup(d->reply_ref);
		t->reply_to = strdup(to);
		if (t->initial)
			snprintf(t->initial, sz, "Subject: %s\n---\n%s", sub,
				 d->body);
		t->shown = original_of(ctx, cfg, t->reply_ref);
	} else if (t->initial) {
		snprintf(t->initial, sz, "To: %s\nSubject: %s\n---\n%s", to,
			 sub, d->body);
	}
	shyake_free_mail_detail(d);
	return 0;
}

static int cmd_compose(int argc, char *argv[], const app_config *cfg,
		       const char *config_dir)
{
	const char *draft_id = NULL;
	if (argc >= 3 && argv[2][0] != '-')
		draft_id = argv[2];

	shyake_config c = lib_config(cfg, config_dir);
	shyake_ctx *ctx = shyake_init_ctx(&c);

	/* build template; editing decrypts the existing draft */
	struct template t = { 0 };
	if (!draft_id) {
		t.initial = strdup("To: \nSubject: \n---\n");
	} else if (prompt_passphrase(ctx, config_dir) != 0 ||
		   draft_template(ctx, cfg, config_dir, draft_id, &t) != 0) {
		shyake_free_ctx(ctx);
		return EXIT_FAILURE;
	}

	char *buf = t.initial ? cli_edit_draft(cfg, config_dir, t.initial,
					       t.shown) :
				NULL;
	int status = buf ? save_edited_draft(ctx, config_dir, buf, t.reply_ref,
					     t.reply_to, draft_id, NULL) :
			   EXIT_FAILURE;
	free(buf);
	free_template(&t);
	shyake_free_ctx(ctx);
	return status;
}

/* ------------------------------------------------------------------ */
/* Reading mail                                                       */
/* ------------------------------------------------------------------ */

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

static int cmd_check(int argc, char *argv[], const app_config *cfg,
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

static int cmd_fetch(int argc, char *argv[], const app_config *cfg,
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

static int cmd_read(int argc, char *argv[], const app_config *cfg,
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

static int cmd_save(int argc, char *argv[], const app_config *cfg,
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

static int cmd_burn(int argc, char *argv[], const app_config *cfg,
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

/* ------------------------------------------------------------------ */
/* Threads                                                            */
/* ------------------------------------------------------------------ */

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
	cli_render_thread_mails(t, d, flat, no_color(cfg), global_plain,
				cfg->tz_hours, cfg->time_format,
				cfg->time_format_recent);
	for (int i = 0; d && i < t->count; i++)
		shyake_free_mail_detail(d[i]);
	free(d);
}

/* check, fetch, save or read thread <id> */
static int cmd_thread(int argc, char *argv[], const app_config *cfg,
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

/* ------------------------------------------------------------------ */
/* Files and updates                                                  */
/* ------------------------------------------------------------------ */

static int cmd_enc(int argc, char *argv[], const app_config *cfg,
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

static int cmd_dec(int argc, char *argv[], const app_config *cfg,
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
	shyake_err ret = shyake_dec_file(ctx, in_path, out_path);
	if (ret != SHYAKE_OK)
		print_lib_error(ctx, "Decryption failed.");
	else if (out_path)
		fprintf(stderr, "Decrypted: %s\n", out_path);
	shyake_free_ctx(ctx);
	return ret == SHYAKE_OK ? EXIT_SUCCESS : EXIT_FAILURE;
}

/* update without arguments: the versions, and how to update */
static int show_versions(const char *version_url)
{
	cli_version_info *info =
		cli_get_latest_version(version_url, global_debug);
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

static int cmd_update(int argc, char *argv[], const app_config *cfg,
		      const char *config_dir)
{
	(void)config_dir;
	char *version_url = build_version_url(cfg->instance);
	if (!version_url)
		return EXIT_FAILURE;

	int ret;
	if (argc < 3)
		ret = show_versions(version_url);
	else if (strcmp(argv[2], "stable") == 0)
		ret = cli_self_update(version_url, SHYAKE_VERSION,
				      CLI_UPDATE_STABLE, global_debug);
	else if (strcmp(argv[2], "preview") == 0)
		ret = cli_self_update(version_url, SHYAKE_VERSION,
				      CLI_UPDATE_PREVIEW, global_debug);
	else {
		fprintf(stderr, "Usage: shyake update [stable|preview]\n");
		ret = EXIT_FAILURE;
	}
	free(version_url);
	return ret == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

/* ------------------------------------------------------------------ */
/* Dispatch                                                           */
/* ------------------------------------------------------------------ */

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
static void take_global_flags(int *argc, char *argv[])
{
	for (int i = 1; i < *argc; i++) {
		int n = 1;
		if (strcmp(argv[i], "--plain") == 0) {
			global_plain = 1;
			global_no_color = 1;
		} else if (strcmp(argv[i], "--debug") == 0) {
			global_debug = 1;
		} else if (strcmp(argv[i], "--no-color") == 0) {
			global_no_color = 1;
		} else if ((strcmp(argv[i], "-c") == 0 ||
			    strcmp(argv[i], "--config") == 0) &&
			   i + 1 < *argc) {
			global_config_dir = argv[i + 1];
			n = 2;
		} else {
			continue;
		}
		drop_args(argc, argv, i, n);
		i--;
	}
}

static char *config_dir_path(void)
{
	return global_config_dir ? strdup(global_config_dir) : get_config_dir();
}

/* no command: DEFAULT_ACTION picks check inbox, its count, or man */
static char **default_argv(int *argc)
{
	static char *def_argv_man[] = { "shyake", "man", NULL };
	static char *def_argv_check[] = { "shyake", "check", "inbox", NULL };
	static char *def_argv_count[] = { "shyake", "check", "inbox", "--count",
					  NULL };
	char *config_dir = config_dir_path();
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
static int run_early(int argc, char *argv[], int *ret)
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
		*ret = cmd_init(global_config_dir) == 0 ? EXIT_SUCCESS :
							  EXIT_FAILURE;
	else
		return 0;
	return 1;
}

int main(int argc, char *argv[])
{
	take_global_flags(&argc, argv);
	const char *no_color_env = getenv("NO_COLOR");
	if (no_color_env && strlen(no_color_env) > 0)
		global_no_color = 1;
	if (argc < 2)
		argv = default_argv(&argc);

	int ret;
	if (run_early(argc, argv, &ret))
		return ret;

	const char *cmd = argv[1];
	const struct command *c = find_command(cmd);
	char *config_dir = config_dir_path();
	app_config *cfg = cli_read_config(config_dir);
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
