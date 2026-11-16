/* Writing mail: send, reply, compose, and the drafts they leave */
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

/* "Re: <subject>", kept within SUBJECT_MAX on a UTF-8 boundary */
static char *reply_subject(const char *orig)
{
	const char *prefix = strncasecmp(orig, "Re:", 3) == 0 ? "" : "Re: ";
	usize n = strlen(prefix) + strlen(orig);
	if (n > SUBJECT_MAX) {
		n = SUBJECT_MAX;
		while (n > strlen(prefix) &&
		       ((u8)orig[n - strlen(prefix)] & 0xC0) == 0x80)
			n--;
	}
	char *s = malloc(n + 1);
	if (s)
		snprintf(s, n + 1, "%s%s", prefix, orig);
	return s;
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

	if (subject && strlen(subject) > SUBJECT_MAX)
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
	if (strlen(subject) > SUBJECT_MAX) {
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

int cmd_send(int argc, char *argv[], const app_config *cfg,
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

int cmd_reply(int argc, char *argv[], const app_config *cfg,
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

int cmd_compose(int argc, char *argv[], const app_config *cfg,
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
