#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <getopt.h>
#include <ctype.h>
#include <sys/wait.h>
#include "shyake.h"
#include "display.h"
#include "prompt.h"
#include "update.h"
#include "drafts.h"
#include "man.h"
#include "internal.h"

#ifndef SHYAKE_VERSION
#define SHYAKE_VERSION "dev"
#endif
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

int cmd_init(const char *config_dir);

char *get_config_dir(void);

static u8 *read_all_bytes(FILE *f, usize *out_len)
{
	usize capacity = 1024;
	usize length = 0;
	u8 *buffer = malloc(capacity);
	if (!buffer)
		return NULL;

	while (!feof(f) && !ferror(f)) {
		if (length == capacity) {
			capacity *= 2;
			u8 *new_buffer = realloc(buffer, capacity);
			if (!new_buffer) {
				free(buffer);
				return NULL;
			}
			buffer = new_buffer;
		}
		usize read_bytes =
			fread(buffer + length, 1, capacity - length, f);
		length += read_bytes;
	}
	*out_len = length;
	return buffer;
}

int global_plain = 0;
int global_debug = 0;
int global_no_color = 0;
char *global_config_dir = NULL;

typedef struct {
	char *instance;
	char *username;
	char *time_format;
	char *time_format_recent;
	char *check_columns;
	char *editor;
	int no_color;
	int tz_hours;
	int default_action;
} app_config;

static char *trim_whitespace(char *str)
{
	// trim leading and trailing spaces
	while (*str == ' ' || *str == '\t' || *str == '\r' || *str == '\n')
		str++;
	if (*str == '\0')
		return str;
	char *end = str + strlen(str) - 1;
	while (end > str &&
	       (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n'))
		end--;
	end[1] = '\0';
	return str;
}

static void parse_check_columns(const char *spec, int *col_order,
				int *col_count)
{
	static const int default_order[] = { COL_ID, COL_PARTY, COL_SUBJECT,
					     COL_SIZE, COL_DATE };
	if (!spec || spec[0] == '\0') {
		for (int i = 0; i < 5; i++)
			col_order[i] = default_order[i];
		*col_count = 5;
		return;
	}

	*col_count = 0;
	char buf[256];
	snprintf(buf, sizeof(buf), "%s", spec);
	char *rest = buf;
	while (rest && *col_count < 5) {
		char *tok = rest;
		char *comma = strchr(rest, ',');
		if (comma) {
			*comma = '\0';
			rest = comma + 1;
		} else {
			rest = NULL;
		}
		while (*tok == ' ')
			tok++;
		int col = 0;
		if (strcmp(tok, "id") == 0)
			col = COL_ID;
		else if (strcmp(tok, "sender") == 0 ||
			 strcmp(tok, "from") == 0 || strcmp(tok, "to") == 0 ||
			 strcmp(tok, "recipient") == 0)
			col = COL_PARTY;
		else if (strcmp(tok, "subject") == 0)
			col = COL_SUBJECT;
		else if (strcmp(tok, "size") == 0)
			col = COL_SIZE;
		else if (strcmp(tok, "date") == 0)
			col = COL_DATE;

		if (col)
			col_order[(*col_count)++] = col;
	}

	if (*col_count == 0) {
		for (int i = 0; i < 5; i++)
			col_order[i] = default_order[i];
		*col_count = 5;
	}
}

static void free_app_config(app_config *config)
{
	// free configuration fields
	if (!config)
		return;
	free(config->instance);
	free(config->username);
	free(config->time_format);
	free(config->time_format_recent);
	free(config->check_columns);
	free(config->editor);
	free(config);
}

static app_config *read_config(const char *config_dir)
{
	// read and parse config file
	app_config *cfg = calloc(1, sizeof(app_config));
	if (!cfg)
		return NULL;
	cfg->tz_hours = TZ_AUTO; // default: system localtime

	char path[512];
	snprintf(path, sizeof(path), "%s/config", config_dir);
	FILE *f = fopen(path, "r");
	if (!f)
		return cfg;

	char line[1024];
	while (fgets(line, sizeof(line), f)) {
		char *trimmed = trim_whitespace(line);
		if (trimmed[0] == '#' || trimmed[0] == '\0')
			continue;

		char *equals = strchr(trimmed, '=');
		if (!equals)
			continue;

		*equals = '\0';
		char *key = trim_whitespace(trimmed);
		char *val = trim_whitespace(equals + 1);

		if ((val[0] == '"' || val[0] == '\'') &&
		    val[0] == val[strlen(val) - 1] && strlen(val) >= 2) {
			val[strlen(val) - 1] = '\0';
			val++;
		}

		if (strcmp(key, "INSTANCE") == 0) {
			free(cfg->instance);
			cfg->instance = strdup(val);
		} else if (strcmp(key, "USERNAME") == 0) {
			free(cfg->username);
			cfg->username = strdup(val);
		} else if (strcmp(key, "TIME_FORMAT") == 0) {
			free(cfg->time_format);
			cfg->time_format = strdup(val);
		} else if (strcmp(key, "TIME_FORMAT_RECENT") == 0) {
			free(cfg->time_format_recent);
			cfg->time_format_recent = strdup(val);
		} else if (strcmp(key, "CHECK_COLUMNS") == 0) {
			free(cfg->check_columns);
			cfg->check_columns = strdup(val);
		} else if (strcmp(key, "NO_COLOR") == 0) {
			cfg->no_color = atoi(val);
		} else if (strcmp(key, "TIME_ZONE") == 0) {
			// auto    = system localtime
			// integer = UTC offset hours
			if (strcmp(val, "auto") == 0 || val[0] == '\0')
				cfg->tz_hours = TZ_AUTO;
			else
				cfg->tz_hours = atoi(val);
		} else if (strcmp(key, "DEFAULT_ACTION") == 0) {
			cfg->default_action = atoi(val);
		} else if (strcmp(key, "EDITOR") == 0) {
			free(cfg->editor);
			cfg->editor = strdup(val);
		}
	}
	fclose(f);
	return cfg;
}

static int update_config_user_and_instance(const char *config_dir,
					   const char *username,
					   const char *instance)
{
	// update INSTANCE key in config file in-place
	char path[512];
	snprintf(path, sizeof(path), "%s/config", config_dir);

	FILE *f = fopen(path, "r");
	char **lines = NULL;
	int count = 0;
	if (f) {
		char line_buf[1024];
		while (fgets(line_buf, sizeof(line_buf), f)) {
			lines = realloc(lines, sizeof(char *) * (count + 1));
			lines[count++] = strdup(line_buf);
		}
		fclose(f);
	}

	int has_instance = 0;
	for (int i = 0; i < count; i++) {
		char line_copy[1024];
		snprintf(line_copy, sizeof(line_copy), "%s", lines[i]);
		char *trimmed = trim_whitespace(line_copy);
		if (trimmed[0] == '#' || trimmed[0] == '\0')
			continue;
		char *equals = strchr(trimmed, '=');
		if (!equals)
			continue;
		*equals = '\0';
		char *key = trim_whitespace(trimmed);
		if (strcmp(key, "INSTANCE") == 0) {
			free(lines[i]);
			char new_line[1024];
			snprintf(new_line, sizeof(new_line),
				 "INSTANCE=%s\nUSERNAME=%s\n", instance,
				 username);
			lines[i] = strdup(new_line);
			has_instance = 1;
		} else if (strcmp(key, "USERNAME") == 0) {
			free(lines[i]);
			lines[i] = strdup(""); // clear old USERNAME line
		}
	}

	f = fopen(path, "w");
	if (!f) {
		for (int i = 0; i < count; i++)
			free(lines[i]);
		free(lines);
		return -1;
	}

	for (int i = 0; i < count; i++) {
		fputs(lines[i], f);
		free(lines[i]);
	}
	free(lines);

	if (!has_instance)
		fprintf(f, "INSTANCE=%s\n", instance);

	fclose(f);
	return 0;
}

/* editor resolution: config > $VISUAL > $EDITOR > ed */
static const char *resolve_editor(const app_config *cfg)
{
	if (cfg->editor && cfg->editor[0])
		return cfg->editor;
	const char *e = getenv("VISUAL");
	if (e && e[0])
		return e;
	e = getenv("EDITOR");
	if (e && e[0])
		return e;
	return "ed";
}

/* parse To:/Subject:/---/body template in place */
static int parse_compose_buffer(char *buf, char **to, char **subject,
				char **body)
{
	*to = NULL;
	*subject = NULL;
	*body = NULL;
	char *line = buf;
	while (line) {
		char *nl = strchr(line, '\n');
		char *next = nl ? nl + 1 : NULL;
		if (nl)
			*nl = '\0';
		char *trimmed = trim_whitespace(line);
		if (strcmp(trimmed, "---") == 0) {
			*body = next ? next : line + strlen(line);
			return 0;
		}
		if (strncmp(trimmed, "To:", 3) == 0)
			*to = trim_whitespace(trimmed + 3);
		else if (strncmp(trimmed, "Subject:", 8) == 0)
			*subject = trim_whitespace(trimmed + 8);
		line = next;
	}
	return -1;
}

/* vim and nvim: split windows, hardening options */
static int is_vim(const char *editor)
{
	const char *base = strrchr(editor, '/');
	base = base ? base + 1 : editor;
	return strcmp(base, "vim") == 0 || strcmp(base, "nvim") == 0;
}

/* editors that open a file at a line with "+<line> <file>" */
static int takes_plus_line(const char *base)
{
	static const char *const names[] = { "vi",    "vim.tiny", "nano",
					     "pico",  "emacs",	  "mg",
					     "micro", "joe" };
	for (usize i = 0; i < sizeof(names) / sizeof(*names); i++)
		if (strcmp(base, names[i]) == 0)
			return 1;
	return 0;
}

/*
 * Run editor on path, with the cursor on line when it is not 0.
 * vim keeps no swap, viminfo, undo or backup file; with beside, it
 * shows that file read-only on the left.
 */
static int launch_editor(const char *editor, const char *path,
			 const char *beside, int line)
{
	const char *base = strrchr(editor, '/');
	base = base ? base + 1 : editor;
	char at[32] = "";
	if (line > 0 && (is_vim(editor) || takes_plus_line(base)))
		snprintf(at, sizeof(at), " +%d", line);

	char cmd[2048];
	if (is_vim(editor) && beside)
		snprintf(cmd, sizeof(cmd),
			 "%s -n -i NONE --cmd 'set noundofile nobackup "
			 "nowritebackup' -O '%s' '%s' -c 'setlocal readonly "
			 "nomodifiable' -c 'wincmd l' -c 'call cursor(%d, 1)'",
			 editor, beside, path, line > 0 ? line : 1);
	else if (is_vim(editor))
		snprintf(cmd, sizeof(cmd),
			 "%s -n -i NONE --cmd 'set noundofile nobackup "
			 "nowritebackup'%s '%s'",
			 editor, at, path);
	else if (strcmp(base, "vim.tiny") == 0)
		snprintf(cmd, sizeof(cmd), "%s -n -i NONE%s '%s'", editor, at,
			 path);
	else
		snprintf(cmd, sizeof(cmd), "%s%s '%s'", editor, at, path);
	int status = system(cmd);
	if (status == -1 || !WIFEXITED(status))
		return -1;
	return WEXITSTATUS(status);
}

/* zero-overwrite and remove a plaintext temp file */
static void shred_and_unlink(const char *path)
{
	FILE *f = fopen(path, "r+");
	if (f) {
		fseek(f, 0, SEEK_END);
		long len = ftell(f);
		fseek(f, 0, SEEK_SET);
		for (long i = 0; i < len; i++)
			fputc(0, f);
		fflush(f);
		fclose(f);
	}
	unlink(path);
}

/* private temp file inside the config dir (not /tmp) */
static int write_temp(const char *config_dir, const char *name,
		      const char *text, char *path, usize path_len)
{
	snprintf(path, path_len, "%s/.%s-XXXXXX", config_dir, name);
	int fd = mkstemp(path);
	if (fd < 0)
		return -1;
	FILE *f = fdopen(fd, "w");
	if (!f) {
		close(fd);
		unlink(path);
		return -1;
	}
	fputs(text, f);
	fclose(f);
	return 0;
}

/* whole file as a NUL-terminated string */
static char *read_text(const char *path)
{
	FILE *f = fopen(path, "rb");
	if (!f)
		return NULL;
	usize len = 0;
	char *buf = (char *)read_all_bytes(f, &len);
	fclose(f);
	if (!buf)
		return NULL;
	char *nbuf = realloc(buf, len + 1);
	if (!nbuf) {
		free(buf);
		return NULL;
	}
	nbuf[len] = '\0';
	return nbuf;
}

/* this line and everything above it are dropped */
#define SCISSORS "# ------------------------ >8 ------------------------"
#define SCISSORS_NOTE                                    \
	"# Do not modify or remove the scissors line.\n" \
	"# Everything above it will be ignored.\n"

/* the mail answered, as fetch shows it, for the editor */
static char *format_original(const shyake_mail_detail *d, const app_config *cfg)
{
	char date[64];
	cli_format_timestamp(d->timestamp, cfg->tz_hours, cfg->time_format,
			     cfg->time_format_recent, date, sizeof(date));
	const char *sub = d->subject ? d->subject : "(decryption failed)";
	const char *body = d->body ? d->body : "(decryption failed)";
	usize n = strlen(d->sender) + strlen(d->recipient) + strlen(date) +
		  strlen(sub) + strlen(body) + 64;
	char *s = malloc(n);
	if (s)
		snprintf(s, n,
			 "FROM:  %s\nTO:    %s\nDATE:  %s\nSUBJ:  %s\n\n%s",
			 d->sender, d->recipient, date, sub, body);
	return s;
}

/* the original, a scissors line, then the template */
static char *with_scissors(const char *initial, const char *orig)
{
	usize n = strlen(orig) + strlen(SCISSORS_NOTE) + strlen(SCISSORS) +
		  strlen(initial) + 8;
	for (const char *p = orig; *p; p++)
		n += *p == '\n';
	char *s = malloc(n);
	if (!s)
		return NULL;
	char *w = s;
	/* a scissors line inside the mail must not cut there */
	for (const char *line = orig; *line;) {
		const char *nl = strchr(line, '\n');
		usize len = nl ? (usize)(nl - line) : strlen(line);
		if (strncmp(line, SCISSORS, strlen(SCISSORS)) == 0)
			*w++ = ' ';
		memcpy(w, line, len);
		w += len;
		*w++ = '\n';
		line += len + (nl ? 1 : 0);
	}
	snprintf(w, n - (usize)(w - s), "\n%s%s\n%s", SCISSORS_NOTE, SCISSORS,
		 initial);
	return s;
}

/* text after the scissors line, or NULL when it is gone */
static const char *after_scissors(const char *buf)
{
	for (const char *line = buf; line && *line;) {
		const char *nl = strchr(line, '\n');
		if (strncmp(line, SCISSORS, strlen(SCISSORS)) == 0)
			return nl ? nl + 1 : line + strlen(line);
		line = nl ? nl + 1 : NULL;
	}
	return NULL;
}

/* line number of the body: the line after the first "---" */
static int body_line(const char *text)
{
	const char *start = after_scissors(text);
	if (!start)
		start = text;
	int n = 1;
	for (const char *p = text; p < start; p++)
		n += *p == '\n';
	for (const char *line = start; line && *line; n++) {
		const char *nl = strchr(line, '\n');
		usize len = nl ? (usize)(nl - line) : strlen(line);
		if (len == 3 && strncmp(line, "---", 3) == 0)
			return n + 1;
		line = nl ? nl + 1 : NULL;
	}
	return 0;
}

/*
 * Let the user edit a draft template. orig, the mail answered, is
 * shown beside it in vim/nvim and above a scissors line elsewhere.
 * Returns the edited text without the scissors part, or NULL when
 * the edit is aborted (the reason is printed).
 */
static char *edit_draft(const app_config *cfg, const char *config_dir,
			const char *initial, const char *orig)
{
	const char *editor = resolve_editor(cfg);
	int split = orig && is_vim(editor);
	char *full = orig && !split ? with_scissors(initial, orig) :
				      strdup(initial);
	char draft_path[640], orig_path[640];
	if (!full || write_temp(config_dir, "compose", full, draft_path,
				sizeof(draft_path)) != 0) {
		fprintf(stderr, "Edit failed. Cannot create a temporary "
				"file.\n");
		free(full);
		return NULL;
	}
	if (split && write_temp(config_dir, "original", orig, orig_path,
				sizeof(orig_path)) != 0)
		split = 0;

	int est = launch_editor(editor, draft_path, split ? orig_path : NULL,
				orig ? body_line(full) : 0);
	char *buf = est == 0 ? read_text(draft_path) : NULL;
	shred_and_unlink(draft_path);
	if (split)
		shred_and_unlink(orig_path);

	if (est != 0) {
		fprintf(stderr,
			"Editor '%s' exited with status %d. Draft aborted.\n",
			editor, est);
	} else if (!buf || strcmp(buf, full) == 0) {
		fprintf(stderr, "Draft aborted.\n");
		free(buf);
		buf = NULL;
	} else if (orig && !split) {
		const char *rest = after_scissors(buf);
		if (rest)
			memmove(buf, rest, strlen(rest) + 1);
		else
			fprintf(stderr,
				"WARNING: The scissors line is gone. The quoted "
				"mail stays in the draft; fix it with 'shyake "
				"compose <id>'.\n");
	}
	free(full);
	if (est == 0 && buf && buf[0] == '\0')
		fprintf(stderr, "Draft aborted.\n");
	if (est != 0 || !buf || buf[0] == '\0') {
		free(buf);
		return NULL;
	}
	return buf;
}

/*
 * Parse an edited template and store it as a draft. A reply's
 * draft keeps reply_to as its recipient, whatever the template says.
 * Returns an exit status.
 */
static int save_edited_draft(shyake_ctx *ctx, const char *config_dir, char *buf,
			     const char *reply_ref, const char *reply_to,
			     const char *draft_id)
{
	char *to, *subject, *body;
	if (parse_compose_buffer(buf, &to, &subject, &body) != 0) {
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
	else if (reply_ref)
		printf("Draft %s saved. Send it with: shyake send -d %s\n", id,
		       id);
	else
		printf("Draft %s saved.\n", id);
	free(new_id);
	return ret == SHYAKE_OK ? EXIT_SUCCESS : EXIT_FAILURE;
}

int main(int argc, char *argv[])
{
	/* parse global flags before command dispatch */
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--plain") == 0) {
			global_plain = 1;
			global_no_color = 1;
			for (int j = i; j < argc - 1; j++)
				argv[j] = argv[j + 1];
			argc--;
			i--;
		} else if (strcmp(argv[i], "--debug") == 0) {
			global_debug = 1;
			for (int j = i; j < argc - 1; j++)
				argv[j] = argv[j + 1];
			argc--;
			i--;
		} else if (strcmp(argv[i], "--no-color") == 0) {
			global_no_color = 1;
			for (int j = i; j < argc - 1; j++)
				argv[j] = argv[j + 1];
			argc--;
			i--;
		} else if ((strcmp(argv[i], "-c") == 0 ||
			    strcmp(argv[i], "--config") == 0) &&
			   i + 1 < argc) {
			global_config_dir = argv[i + 1];
			for (int j = i; j < argc - 2; j++)
				argv[j] = argv[j + 2];
			argc -= 2;
			i--;
		}
	}

	const char *no_color_env = getenv("NO_COLOR");
	if (no_color_env && strlen(no_color_env) > 0)
		global_no_color = 1;

	static char *def_argv_man[] = { "shyake", "man", NULL };
	static char *def_argv_check[] = { "shyake", "check", "inbox", NULL };
	static char *def_argv_count[] = { "shyake", "check", "inbox", "--count",
					  NULL };

	if (argc < 2) {
		char *config_dir = global_config_dir ?
					   strdup(global_config_dir) :
					   get_config_dir();
		app_config *app_cfg = read_config(config_dir);
		int def_act = app_cfg->default_action;
		free_app_config(app_cfg);
		free(config_dir);

		if (def_act == 1) {
			argv = def_argv_check;
			argc = 3;
		} else if (def_act == 2) {
			argv = def_argv_count;
			argc = 4;
		} else {
			argv = def_argv_man;
			argc = 2;
		}
	}

	const char *cmd = argv[1];

	// handle --version/-v/-V
	if (strcmp(cmd, "--version") == 0 || strcmp(cmd, "-v") == 0 ||
	    strcmp(cmd, "-V") == 0) {
		printf("Use 'shyake version' instead.\n");
		return EXIT_SUCCESS;
	}

	// handle help commands
	if (strcmp(cmd, "help") == 0 || strcmp(cmd, "--help") == 0 ||
	    strcmp(cmd, "-h") == 0) {
		printf("Use 'shyake man' instead.\n");
		return EXIT_SUCCESS;
	}

	// handle man command
	if (strcmp(cmd, "man") == 0) {
		cli_print_man(argc < 3 ? NULL : argv[2]);
		return EXIT_SUCCESS;
	}

	if (strcmp(cmd, "version") == 0) {
		printf("shyake %s\n", SHYAKE_VERSION);
		return EXIT_SUCCESS;
	}

	if (strcmp(cmd, "init") == 0) {
		int ret = cmd_init(global_config_dir);
		return ret == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
	}

	char *config_dir = global_config_dir ? strdup(global_config_dir) :
					       get_config_dir();
	app_config *app_cfg = read_config(config_dir);

	if (strcmp(cmd, "whoami") == 0) {
		const char *inst = app_cfg->instance;
		if (app_cfg->username) {
			printf("USERNAME: %s\n", app_cfg->username);
		} else {
			printf("USERNAME: (not registered)\n");
		}
		printf("INSTANCE: %s\n", inst);
		printf("CONFIG:   %s\n", config_dir);
		free_app_config(app_cfg);
		free(config_dir);
		return EXIT_SUCCESS;
	}

	if (strcmp(cmd, "register") == 0) {
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
			switch (opt) {
			case 'u':
				username = optarg;
				break;
			case 'i':
				instance = optarg;
				break;
			default:
				break;
			}
		}

		if (!username) {
			fprintf(stderr, "A username is required.\n"
					"Usage: shyake register -u <username> "
					"-i <url>\n");
			free_app_config(app_cfg);
			free(config_dir);
			return EXIT_FAILURE;
		}

		const char *inst = instance ? instance : (app_cfg->instance);

		if (!inst) {
			fprintf(stderr, "Missing INSTANCE in config file.\n");
			free_app_config(app_cfg);
			free(config_dir);
			return EXIT_FAILURE;
		}
		shyake_config cfg = { .config_dir = config_dir,
				      .instance_url = inst,
				      .username = username,
				      .plain = global_plain,
				      .debug = global_debug,
				      .no_color = global_no_color ||
						  app_cfg->no_color };

		shyake_ctx *ctx = shyake_init_ctx(&cfg);
		if (prompt_passphrase(ctx, config_dir) != 0) {
			shyake_free_ctx(ctx);
			free_app_config(app_cfg);
			free(config_dir);
			return EXIT_FAILURE;
		}
		fprintf(stderr, "Registering as %s at %s... ", username, inst);
		fflush(stderr);
		shyake_err ret = shyake_register(ctx, username);
		if (ret == SHYAKE_OK) {
			fprintf(stderr, "done.\n");
			printf("Registered.\n");
			update_config_user_and_instance(config_dir, username,
							inst);
		} else if (ret == SHYAKE_ERR_NO_INSTANCE) {
			fprintf(stderr, "\n");
			print_failure("Registration failed.",
				      "No instance is configured.");
		} else {
			fprintf(stderr, "\n");
			print_lib_error(ctx, "Registration failed.");
		}

		shyake_free_ctx(ctx);
		free_app_config(app_cfg);
		free(config_dir);
		return ret == SHYAKE_OK ? EXIT_SUCCESS : EXIT_FAILURE;
	}

	if (strcmp(cmd, "send") == 0) {
		char *recipient = NULL;
		char *subject = NULL;
		char *extracted_subject = NULL;
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
			switch (opt) {
			case 't':
				recipient = optarg;
				break;
			case 's':
				subject = optarg;
				break;
			case 'd':
				draft_id = optarg;
				break;
			default:
				break;
			}
		}

		/* send a stored draft, delete it on success */
		if (draft_id) {
			const char *inst = app_cfg->instance;
			const char *user = app_cfg->username;
			if (!inst || !user) {
				fprintf(stderr,
					"Missing INSTANCE or USERNAME in config file.\n");
				free_app_config(app_cfg);
				free(config_dir);
				return EXIT_FAILURE;
			}
			shyake_config cfg = { .config_dir = config_dir,
					      .instance_url = inst,
					      .username = user,
					      .plain = global_plain,
					      .debug = global_debug,
					      .no_color = global_no_color ||
							  app_cfg->no_color };
			shyake_ctx *ctx = shyake_init_ctx(&cfg);
			if (prompt_passphrase(ctx, config_dir) != 0) {
				shyake_free_ctx(ctx);
				free_app_config(app_cfg);
				free(config_dir);
				return EXIT_FAILURE;
			}

			shyake_mail_detail *d = cli_read_draft(
				ctx, config_dir, app_cfg->username, draft_id);
			if (!d || !d->body) {
				print_drafts_error("Draft read failed.");
				shyake_free_mail_detail(d);
				shyake_free_ctx(ctx);
				free_app_config(app_cfg);
				free(config_dir);
				return EXIT_FAILURE;
			}
			/* a reply goes where its draft says */
			if (d->reply_ref && recipient && d->recipient &&
			    strcmp(recipient, d->recipient) != 0) {
				fprintf(stderr,
					"Draft %s is a reply to %s. "
					"-t cannot change that.\n",
					draft_id, d->recipient);
				shyake_free_mail_detail(d);
				shyake_free_ctx(ctx);
				free_app_config(app_cfg);
				free(config_dir);
				return EXIT_FAILURE;
			}
			if (!recipient && d->recipient && d->recipient[0])
				recipient = d->recipient;
			if (!subject && d->subject && d->subject[0])
				subject = d->subject;

			shyake_err ret = SHYAKE_ERR;
			if (!recipient) {
				fprintf(stderr, "The draft has no recipient. "
						"Use -t <username>.\n");
			} else if (!subject) {
				fprintf(stderr, "The draft has no subject. "
						"Use -s <subject>.\n");
			} else if (strlen(subject) > 128) {
				fprintf(stderr,
					"The subject cannot exceed 128 bytes.\n");
			} else {
				fprintf(stderr, "Sending draft %s to %s... ",
					draft_id, recipient);
				fflush(stderr);
				ret = d->reply_ref ?
					      shyake_send_reply(
						      ctx, recipient,
						      d->reply_ref, subject,
						      (const u8 *)d->body,
						      strlen(d->body)) :
					      shyake_send(ctx, recipient,
							  subject,
							  (const u8 *)d->body,
							  strlen(d->body));
				if (ret == SHYAKE_OK) {
					fprintf(stderr, "done.\n");
					printf("Mail sent.\n");
					if (cli_delete_draft(config_dir,
							     draft_id) ==
					    SHYAKE_OK)
						printf("Draft %s deleted.\n",
						       draft_id);
				} else {
					fprintf(stderr, "\n");
					print_send_error(ctx, ret, recipient);
				}
			}

			shyake_free_mail_detail(d);
			shyake_free_ctx(ctx);
			free_app_config(app_cfg);
			free(config_dir);
			return ret == SHYAKE_OK ? EXIT_SUCCESS : EXIT_FAILURE;
		}

		if (!recipient) {
			fprintf(stderr, "A recipient is required.\n"
					"Usage: shyake send -t <username> "
					"[-s <subject>] [<file>]\n");
			free_app_config(app_cfg);
			free(config_dir);
			return EXIT_FAILURE;
		}

		FILE *in_file = stdin;
		if (optind < argc) {
			in_file = fopen(argv[optind], "rb");
			if (!in_file) {
				fprintf(stderr,
					"Send failed. Cannot open %s.\n",
					argv[optind]);
				free_app_config(app_cfg);
				free(config_dir);
				return EXIT_FAILURE;
			}
		}

		usize body_len;
		u8 *body = read_all_bytes(in_file, &body_len);
		if (in_file != stdin)
			fclose(in_file);

		if (!body) {
			fprintf(stderr, "Send failed. Cannot read the body.\n");
			free_app_config(app_cfg);
			free(config_dir);
			return EXIT_FAILURE;
		}

		if (!subject) {
			u8 *newline = memchr(body, '\n', body_len);
			usize first_line_len =
				newline ? (usize)(newline - body) : body_len;
			usize actual_len = first_line_len;
			if (actual_len > 0 && body[actual_len - 1] == '\r') {
				actual_len--;
			}

			extracted_subject = malloc(actual_len + 1);
			memcpy(extracted_subject, body, actual_len);
			extracted_subject[actual_len] = '\0';
			subject = extracted_subject;

			usize advance = newline ? first_line_len + 1 : body_len;
			body_len -= advance;
			if (body_len > 0) {
				memmove(body, body + advance, body_len);
			}
		}

		if (!subject || strlen(subject) == 0) {
			fprintf(stderr, "The subject cannot be empty.\n");
			if (extracted_subject)
				free(extracted_subject);
			free(body);
			free_app_config(app_cfg);
			free(config_dir);
			return EXIT_FAILURE;
		}
		if (strlen(subject) > 128) {
			fprintf(stderr,
				"The subject cannot exceed 128 bytes.\n");
			if (extracted_subject)
				free(extracted_subject);
			free(body);
			free_app_config(app_cfg);
			free(config_dir);
			return EXIT_FAILURE;
		}
		int is_blank = 1;
		for (usize i = 0; subject[i] != '\0'; i++) {
			if (!isspace((unsigned char)subject[i])) {
				is_blank = 0;
				break;
			}
		}
		if (is_blank) {
			fprintf(stderr,
				"The subject cannot be only whitespace.\n");
			if (extracted_subject)
				free(extracted_subject);
			free(body);
			free_app_config(app_cfg);
			free(config_dir);
			return EXIT_FAILURE;
		}

		const char *inst = app_cfg->instance;
		const char *user = app_cfg->username;

		if (!inst || !user) {
			fprintf(stderr,
				"Missing INSTANCE or USERNAME in config file.\n");
			free_app_config(app_cfg);
			free(config_dir);
			return EXIT_FAILURE;
		}
		shyake_config cfg = { .config_dir = config_dir,
				      .instance_url = inst,
				      .username = user,
				      .plain = global_plain,
				      .debug = global_debug,
				      .no_color = global_no_color ||
						  app_cfg->no_color };

		shyake_ctx *ctx = shyake_init_ctx(&cfg);
		if (prompt_passphrase(ctx, config_dir) != 0) {
			shyake_free_ctx(ctx);
			free_app_config(app_cfg);
			free(config_dir);
			free(body);
			if (extracted_subject)
				free(extracted_subject);
			return EXIT_FAILURE;
		}
		fprintf(stderr, "Sending mail to %s... ", recipient);
		fflush(stderr);
		shyake_err ret =
			shyake_send(ctx, recipient, subject, body, body_len);
		if (ret == SHYAKE_OK) {
			fprintf(stderr, "done.\n");
			printf("Mail sent.\n");
		} else {
			fprintf(stderr, "\n");
			print_send_error(ctx, ret, recipient);
		}

		/* a failed send must not eat what the user wrote; a
		 * retry cannot help when you are blocked or the
		 * recipient is gone */
		if (ret != SHYAKE_OK)
			rescue_draft(ctx, config_dir, recipient, subject, NULL,
				     body, body_len,
				     ret != SHYAKE_ERR_BLOCKED &&
					     ret != SHYAKE_ERR_GONE &&
					     ret != SHYAKE_ERR_NOT_FOUND);

		shyake_free_ctx(ctx);
		free_app_config(app_cfg);
		free(config_dir);
		free(body);
		if (extracted_subject)
			free(extracted_subject);
		return ret == SHYAKE_OK ? EXIT_SUCCESS : EXIT_FAILURE;
	}

	if (strcmp(cmd, "reply") == 0) {
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
			free_app_config(app_cfg);
			free(config_dir);
			return EXIT_FAILURE;
		}
		const char *mail_id = argv[optind++];
		const char *problem = subject ? subject_problem(subject) : NULL;
		if (problem) {
			fprintf(stderr, "%s\n", problem);
			free_app_config(app_cfg);
			free(config_dir);
			return EXIT_FAILURE;
		}

		const char *inst = app_cfg->instance;
		const char *user = app_cfg->username;
		if (!inst || !user) {
			fprintf(stderr,
				"Missing INSTANCE or USERNAME in config file.\n");
			free_app_config(app_cfg);
			free(config_dir);
			return EXIT_FAILURE;
		}

		/* legacy: a body from a file or a pipe is sent at once */
		int direct = !draft_id &&
			     (optind < argc || !isatty(STDIN_FILENO));
		FILE *in_file = stdin;
		if (direct && optind < argc) {
			in_file = fopen(argv[optind], "rb");
			if (!in_file) {
				fprintf(stderr,
					"Reply failed. Cannot open %s.\n",
					argv[optind]);
				free_app_config(app_cfg);
				free(config_dir);
				return EXIT_FAILURE;
			}
		}
		usize body_len = 0;
		u8 *body = NULL;
		if (direct) {
			body = read_all_bytes(in_file, &body_len);
			if (in_file != stdin)
				fclose(in_file);
			if (!body) {
				fprintf(stderr,
					"Reply failed. Cannot read the body.\n");
				free_app_config(app_cfg);
				free(config_dir);
				return EXIT_FAILURE;
			}
		}

		shyake_config cfg = { .config_dir = config_dir,
				      .instance_url = inst,
				      .username = user,
				      .plain = global_plain,
				      .debug = global_debug,
				      .no_color = global_no_color ||
						  app_cfg->no_color };
		shyake_ctx *ctx = shyake_init_ctx(&cfg);
		if (prompt_passphrase(ctx, config_dir) != 0) {
			free(body);
			shyake_free_ctx(ctx);
			free_app_config(app_cfg);
			free(config_dir);
			return EXIT_FAILURE;
		}

		/* the mail answered gives recipient, reference, subject */
		shyake_mail_detail *orig = shyake_fetch(ctx, mail_id);
		const char *recipient =
			orig ? shyake_reply_recipient(ctx, orig) : NULL;
		int status = EXIT_FAILURE;
		if (!orig) {
			print_lib_error(ctx, "Reply failed.");
			goto reply_done;
		}
		if (!orig->ref || !recipient) {
			fprintf(stderr,
				"Reply failed. Mail %s cannot be answered.\n",
				mail_id);
			goto reply_done;
		}
		char *re_subject =
			orig->subject ? reply_subject(orig->subject) : NULL;
		if (!subject)
			subject = re_subject;

		if (draft_id) {
			/* turn a stored draft into this reply */
			shyake_mail_detail *d = cli_read_draft(
				ctx, config_dir, app_cfg->username, draft_id);
			if (!d || !d->body) {
				print_drafts_error("Draft read failed.");
			} else {
				if (d->recipient && d->recipient[0] &&
				    strcmp(d->recipient, recipient) != 0)
					fprintf(stderr,
						"Draft %s now goes to %s.\n",
						draft_id, recipient);
				const char *sub =
					d->subject && d->subject[0] &&
							!subject_problem(
								d->subject) ?
						d->subject :
						subject;
				if (cli_save_draft(ctx, config_dir, recipient,
						   sub, orig->ref,
						   (const u8 *)d->body,
						   strlen(d->body), draft_id,
						   NULL) == SHYAKE_OK) {
					printf("Draft %s is now a reply to "
					       "%s. Send it with: shyake send "
					       "-d %s\n",
					       draft_id, mail_id, draft_id);
					status = EXIT_SUCCESS;
				} else {
					print_drafts_error(
						"Draft save failed.");
				}
			}
			shyake_free_mail_detail(d);
		} else if (direct) {
			if (!subject) {
				fprintf(stderr,
					"Reply failed. The subject of mail %s "
					"cannot be decrypted. "
					"Use -s <subject>.\n",
					mail_id);
			} else {
				fprintf(stderr, "Replying to %s... ",
					recipient);
				fflush(stderr);
				shyake_err ret = shyake_send_reply(
					ctx, recipient, orig->ref, subject,
					body, body_len);
				if (ret == SHYAKE_OK) {
					fprintf(stderr, "done.\n");
					printf("Mail sent.\n");
					status = EXIT_SUCCESS;
				} else {
					fprintf(stderr, "\n");
					print_send_error(ctx, ret, recipient);
					rescue_draft(
						ctx, config_dir, recipient,
						subject, orig->ref, body,
						body_len,
						ret != SHYAKE_ERR_BLOCKED &&
							ret != SHYAKE_ERR_GONE &&
							ret != SHYAKE_ERR_NOT_FOUND);
				}
			}
		} else {
			/* write the reply in the editor, beside the mail */
			const char *sub = subject ? subject : "";
			usize n = strlen(sub) + 32;
			char *initial = malloc(n);
			char *shown = format_original(orig, app_cfg);
			char *buf = NULL;
			if (initial && shown) {
				snprintf(initial, n, "Subject: %s\n---\n\n",
					 sub);
				buf = edit_draft(app_cfg, config_dir, initial,
						 shown);
			}
			if (buf)
				status = save_edited_draft(ctx, config_dir, buf,
							   orig->ref, recipient,
							   NULL);
			free(buf);
			free(initial);
			free(shown);
		}
		free(re_subject);

reply_done:
		shyake_free_mail_detail(orig);
		free(body);
		shyake_free_ctx(ctx);
		free_app_config(app_cfg);
		free(config_dir);
		return status;
	}

	if (strcmp(cmd, "compose") == 0) {
		const char *draft_id = NULL;
		if (argc >= 3 && argv[2][0] != '-')
			draft_id = argv[2];

		shyake_config cfg = {
			.config_dir = config_dir,
			.instance_url = app_cfg->instance ? app_cfg->instance :
							    "",
			.username = app_cfg->username ? app_cfg->username : "",
			.plain = global_plain,
			.debug = global_debug,
			.no_color = global_no_color || app_cfg->no_color
		};
		shyake_ctx *ctx = shyake_init_ctx(&cfg);

		/* build template; editing decrypts the existing draft */
		char *initial = NULL, *shown = NULL;
		char *reply_ref = NULL, *reply_to = NULL;
		if (draft_id) {
			if (prompt_passphrase(ctx, config_dir) != 0) {
				shyake_free_ctx(ctx);
				free_app_config(app_cfg);
				free(config_dir);
				return EXIT_FAILURE;
			}
			shyake_mail_detail *d = cli_read_draft(
				ctx, config_dir, app_cfg->username, draft_id);
			if (!d || !d->body) {
				print_drafts_error("Draft read failed.");
				shyake_free_mail_detail(d);
				shyake_free_ctx(ctx);
				free_app_config(app_cfg);
				free(config_dir);
				return EXIT_FAILURE;
			}
			const char *to = d->recipient ? d->recipient : "";
			const char *sub = d->subject ? d->subject : "";
			usize sz =
				strlen(to) + strlen(sub) + strlen(d->body) + 32;
			initial = malloc(sz);
			if (d->reply_ref) {
				/* a reply: fixed recipient, the mail beside */
				reply_ref = strdup(d->reply_ref);
				reply_to = strdup(to);
				if (initial)
					snprintf(initial, sz,
						 "Subject: %s\n---\n%s", sub,
						 d->body);
				int saved = 0;
				char *oid =
					shyake_find_ref(ctx, reply_ref, &saved);
				shyake_mail_detail *o =
					!oid  ? NULL :
					saved ? shyake_read_saved(ctx, oid) :
						shyake_fetch(ctx, oid);
				if (o)
					shown = format_original(o, app_cfg);
				shyake_free_mail_detail(o);
				free(oid);
			} else if (initial) {
				snprintf(initial, sz,
					 "To: %s\nSubject: %s\n---\n%s", to,
					 sub, d->body);
			}
			shyake_free_mail_detail(d);
		} else {
			initial = strdup("To: \nSubject: \n---\n");
		}

		char *buf = initial ? edit_draft(app_cfg, config_dir, initial,
						 shown) :
				      NULL;
		int status = buf ? save_edited_draft(ctx, config_dir, buf,
						     reply_ref, reply_to,
						     draft_id) :
				   EXIT_FAILURE;
		free(buf);
		free(initial);
		free(shown);
		free(reply_ref);
		free(reply_to);
		shyake_free_ctx(ctx);
		free_app_config(app_cfg);
		free(config_dir);
		return status;
	}

	if (strcmp(cmd, "check") == 0) {
		if (argc < 3) {
			fprintf(stderr,
				"Usage: shyake check <inbox|sent|id> [options]\n");
			free_app_config(app_cfg);
			free(config_dir);
			return EXIT_FAILURE;
		}

		const char *arg = argv[2];
		int is_list =
			(strcmp(arg, "inbox") == 0 || strcmp(arg, "sent") == 0);
		int is_saved = (strcmp(arg, "saved") == 0);
		int is_drafts = (strcmp(arg, "drafts") == 0);

		cli_render_opts ro = { 0 };
		if (is_list) {
			static struct option check_options[] = {
				{ "count", no_argument, 0, 'C' },
				{ "json", no_argument, 0, 'J' },
				{ "csv", no_argument, 0, 'S' },
				{ "no-header", no_argument, 0, 'H' },
				{ 0, 0, 0, 0 }
			};
			int opt, opt_idx = 0;
			optind = 3;
			while ((opt = getopt_long(argc, argv, "", check_options,
						  &opt_idx)) != -1) {
				switch (opt) {
				case 'C':
					ro.count_only = 1;
					break;
				case 'J':
					ro.json_out = 1;
					break;
				case 'S':
					ro.csv_out = 1;
					break;
				case 'H':
					ro.no_header = 1;
					break;
				default:
					break;
				}
			}
		}

		/* handle check drafts [<id>] entirely from local disk */
		if (is_drafts) {
			shyake_config cfg = {
				.config_dir = config_dir,
				.instance_url = app_cfg->instance ?
							app_cfg->instance :
							"",
				.username = app_cfg->username ?
						    app_cfg->username :
						    "",
				.plain = global_plain,
				.debug = global_debug,
				.no_color = global_no_color || app_cfg->no_color
			};
			shyake_ctx *ctx = shyake_init_ctx(&cfg);
			if (prompt_passphrase(ctx, config_dir) != 0) {
				shyake_free_ctx(ctx);
				free_app_config(app_cfg);
				free(config_dir);
				return EXIT_FAILURE;
			}
			int ret = 0;

			if (argc >= 4) {
				/* check drafts <id> — header only, no body */
				shyake_mail_detail *d = cli_read_draft(
					ctx, config_dir, app_cfg->username,
					argv[3]);
				if (d) {
					char *re = describe_reply(ctx, d);
					cli_render_mail_header(
						d, re, cfg.no_color,
						app_cfg->tz_hours,
						app_cfg->time_format,
						app_cfg->time_format_recent);
					free(re);
					shyake_free_mail_detail(d);
				} else {
					print_drafts_error(
						"Draft read failed.");
					ret = -1;
				}
			} else {
				/* check drafts — list all; NULL means key failure */
				shyake_saved_list *slist = cli_list_drafts(
					ctx, config_dir, app_cfg->username);
				if (!slist) {
					print_drafts_error(
						"Listing drafts failed.");
					ret = -1;
				} else if (slist->count > 0) {
					shyake_mail_list mlist;
					mlist.count = slist->count;
					mlist.entries = calloc(
						slist->count,
						sizeof(shyake_mail_entry));

					for (int i = 0; i < slist->count; i++) {
						shyake_saved_entry *se =
							&slist->entries[i];
						shyake_mail_entry *me =
							&mlist.entries[i];
						me->mail_id = se->mail_id;
						me->party =
							se->recipient[0] ?
								se->recipient :
								"(null)";
						me->subject = se->subject;
						me->size = se->size;
						me->timestamp = se->timestamp;
						me->created = se->created;
						me->is_sent =
							1; /* party is the recipient */
					}

					cli_render_opts ro2 = { 0 };
					ro2.no_color = cfg.no_color;
					ro2.plain = cfg.plain;
					ro2.tz_hours = app_cfg->tz_hours;
					ro2.time_fmt = app_cfg->time_format;
					ro2.time_fmt_recent =
						app_cfg->time_format_recent;
					ro2.id_label = "ID";
					/* drafts use Created + Modified instead of Date */
					ro2.col_order[0] = COL_ID;
					ro2.col_order[1] = COL_PARTY;
					ro2.col_order[2] = COL_SUBJECT;
					ro2.col_order[3] = COL_SIZE;
					ro2.col_order[4] = COL_CREATED;
					ro2.col_order[5] = COL_MODIFIED;
					ro2.col_count = 6;
					cli_render_mail_list(&mlist, &ro2);

					free(mlist.entries);
				} else {
					printf("No drafts.\n");
				}
				shyake_free_saved_list(slist);
			}

			shyake_free_ctx(ctx);
			free_app_config(app_cfg);
			free(config_dir);
			return ret == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
		}

		/* handle check saved [<id>] entirely from local disk */
		if (is_saved) {
			const char *user = app_cfg->username;
			if (!user) {
				fprintf(stderr,
					"Missing USERNAME in config file.\n");
				free_app_config(app_cfg);
				free(config_dir);
				return EXIT_FAILURE;
			}
			shyake_config cfg = {
				.config_dir = config_dir,
				.instance_url = app_cfg->instance ?
							app_cfg->instance :
							"",
				.username = user,
				.plain = global_plain,
				.debug = global_debug,
				.no_color = global_no_color || app_cfg->no_color
			};
			shyake_ctx *ctx = shyake_init_ctx(&cfg);
			if (prompt_passphrase(ctx, config_dir) != 0) {
				shyake_free_ctx(ctx);
				free_app_config(app_cfg);
				free(config_dir);
				return EXIT_FAILURE;
			}
			int ret = 0;

			if (argc >= 4) {
				/* check saved <id> */
				const char *saved_id = argv[3];
				shyake_mail_detail *d =
					shyake_check_saved_one(ctx, saved_id);
				if (d) {
					char *re = describe_reply(ctx, d);
					cli_render_mail_header(
						d, re, cfg.no_color,
						app_cfg->tz_hours,
						app_cfg->time_format,
						app_cfg->time_format_recent);
					free(re);
					shyake_free_mail_detail(d);
				} else {
					print_lib_error(ctx, "Read failed.");
					ret = -1;
				}
			} else {
				/* check saved — list all, reuse cli_render_mail_list */
				shyake_saved_list *slist =
					shyake_list_saved(ctx);
				if (slist) {
					/* convert shyake_saved_list -> shyake_mail_list */
					shyake_mail_list mlist;
					mlist.count = slist->count;
					mlist.entries = calloc(
						slist->count,
						sizeof(shyake_mail_entry));

					for (int i = 0; i < slist->count; i++) {
						shyake_saved_entry *se =
							&slist->entries[i];
						shyake_mail_entry *me =
							&mlist.entries[i];
						me->mail_id = se->mail_id;
						me->party = se->sender;
						me->subject = se->subject;
						me->size = se->size;
						me->timestamp = se->timestamp;
						me->is_sent =
							0; /* always show as inbox */
					}

					cli_render_opts ro2 = { 0 };
					ro2.no_color = cfg.no_color;
					ro2.plain = cfg.plain;
					ro2.tz_hours = app_cfg->tz_hours;
					ro2.time_fmt = app_cfg->time_format;
					ro2.time_fmt_recent =
						app_cfg->time_format_recent;
					parse_check_columns(
						app_cfg->check_columns,
						ro2.col_order, &ro2.col_count);
					cli_render_mail_list(&mlist, &ro2);

					free(mlist.entries);
					shyake_free_saved_list(slist);
				} else {
					printf("No saved mail.\n");
				}
			}

			shyake_free_ctx(ctx);
			free_app_config(app_cfg);
			free(config_dir);
			return ret == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
		}

		const char *inst = app_cfg->instance;
		const char *user = app_cfg->username;

		if (!inst || !user) {
			fprintf(stderr,
				"Missing INSTANCE or USERNAME in config file.\n");
			free_app_config(app_cfg);
			free(config_dir);
			return EXIT_FAILURE;
		}
		shyake_config cfg = { .config_dir = config_dir,
				      .instance_url = inst,
				      .username = user,
				      .plain = global_plain,
				      .debug = global_debug,
				      .no_color = global_no_color ||
						  app_cfg->no_color };

		shyake_ctx *ctx = shyake_init_ctx(&cfg);
		if (prompt_passphrase(ctx, config_dir) != 0) {
			shyake_free_ctx(ctx);
			free_app_config(app_cfg);
			free(config_dir);
			return EXIT_FAILURE;
		}
		int ret = 0;

		if (is_list) {
			ro.no_color = cfg.no_color;
			ro.plain = cfg.plain;
			ro.tz_hours = app_cfg->tz_hours;
			ro.time_fmt = app_cfg->time_format;
			ro.time_fmt_recent = app_cfg->time_format_recent;
			parse_check_columns(app_cfg->check_columns,
					    ro.col_order, &ro.col_count);
			shyake_mail_list *list = shyake_check(ctx, arg);
			if (list) {
				cli_render_mail_list(list, &ro);
				shyake_free_mail_list(list);
			} else {
				print_lib_error(ctx, "Check failed.");
				ret = -1;
			}
		} else {
			/* check <id> metadata view */
			shyake_mail_detail *d = shyake_check_one(ctx, arg);
			if (d) {
				char *re = describe_reply(ctx, d);
				cli_render_mail_header(
					d, re, cfg.no_color, app_cfg->tz_hours,
					app_cfg->time_format,
					app_cfg->time_format_recent);
				free(re);
				shyake_free_mail_detail(d);
			} else {
				print_lib_error(ctx, "Check failed.");
				ret = -1;
			}
		}

		shyake_free_ctx(ctx);
		free_app_config(app_cfg);
		free(config_dir);
		return ret == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
	}

	if (strcmp(cmd, "fetch") == 0) {
		int raw = 0;
		const char *mail_id = NULL;

		static struct option long_options[] = {
			{ "raw", no_argument, 0, 'r' }, { 0, 0, 0, 0 }
		};

		int opt, option_index = 0;
		optind = 2;
		while ((opt = getopt_long(argc, argv, "r", long_options,
					  &option_index)) != -1) {
			switch (opt) {
			case 'r':
				raw = 1;
				break;
			default:
				break;
			}
		}

		if (optind < argc) {
			mail_id = argv[optind];
		}

		if (!mail_id) {
			fprintf(stderr, "A mail ID is required.\n"
					"Usage: shyake fetch <id>\n");
			free_app_config(app_cfg);
			free(config_dir);
			return EXIT_FAILURE;
		}

		const char *inst = app_cfg->instance;
		const char *user = app_cfg->username;

		if (!inst || !user) {
			fprintf(stderr,
				"Missing INSTANCE or USERNAME in config file.\n");
			free_app_config(app_cfg);
			free(config_dir);
			return EXIT_FAILURE;
		}
		shyake_config cfg = { .config_dir = config_dir,
				      .instance_url = inst,
				      .username = user,
				      .plain = global_plain,
				      .debug = global_debug,
				      .no_color = global_no_color ||
						  app_cfg->no_color };

		shyake_ctx *ctx = shyake_init_ctx(&cfg);
		if (prompt_passphrase(ctx, config_dir) != 0) {
			shyake_free_ctx(ctx);
			free_app_config(app_cfg);
			free(config_dir);
			return EXIT_FAILURE;
		}
		int ret = 0;
		shyake_mail_detail *d = shyake_fetch(ctx, mail_id);
		if (d) {
			char *re = raw ? NULL : describe_reply(ctx, d);
			cli_render_mail_detail(d, re, raw, cfg.no_color,
					       cfg.plain, app_cfg->tz_hours,
					       app_cfg->time_format,
					       app_cfg->time_format_recent);
			free(re);
			shyake_free_mail_detail(d);
		} else {
			print_lib_error(ctx, "Fetch failed.");
			ret = -1;
		}

		shyake_free_ctx(ctx);
		free_app_config(app_cfg);
		free(config_dir);
		return ret == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
	}

	if (strcmp(cmd, "burn") == 0) {
		if (argc < 3) {
			fprintf(stderr, "Usage: shyake burn <id>\n");
			free_app_config(app_cfg);
			free(config_dir);
			return EXIT_FAILURE;
		}
		const char *mail_id = argv[2];
		const char *inst = app_cfg->instance;
		const char *user = app_cfg->username;

		if (!inst || !user) {
			fprintf(stderr,
				"Missing INSTANCE or USERNAME in config file.\n");
			free_app_config(app_cfg);
			free(config_dir);
			return EXIT_FAILURE;
		}
		shyake_config cfg = { .config_dir = config_dir,
				      .instance_url = inst,
				      .username = user,
				      .plain = global_plain,
				      .debug = global_debug,
				      .no_color = global_no_color ||
						  app_cfg->no_color };
		shyake_ctx *ctx = shyake_init_ctx(&cfg);
		if (prompt_passphrase(ctx, config_dir) != 0) {
			shyake_free_ctx(ctx);
			free_app_config(app_cfg);
			free(config_dir);
			return EXIT_FAILURE;
		}
		shyake_err ret = shyake_burn(ctx, mail_id);
		if (ret == SHYAKE_OK)
			printf("Mail burned.\n");
		else
			print_lib_error(ctx, "Burn failed.");
		shyake_free_ctx(ctx);
		free_app_config(app_cfg);
		free(config_dir);
		return ret == SHYAKE_OK ? EXIT_SUCCESS : EXIT_FAILURE;
	}

	if (strcmp(cmd, "block") == 0 || strcmp(cmd, "unblock") == 0) {
		int is_unblock = strcmp(cmd, "unblock") == 0;
		if (argc < 3) {
			fprintf(stderr, "Usage: shyake %s <target>\n", cmd);
			free_app_config(app_cfg);
			free(config_dir);
			return EXIT_FAILURE;
		}
		const char *target = argv[2];
		const char *inst = app_cfg->instance;
		const char *user = app_cfg->username;

		if (!inst || !user) {
			fprintf(stderr,
				"Missing INSTANCE or USERNAME in config file.\n");
			free_app_config(app_cfg);
			free(config_dir);
			return EXIT_FAILURE;
		}
		shyake_config cfg = { .config_dir = config_dir,
				      .instance_url = inst,
				      .username = user,
				      .plain = global_plain,
				      .debug = global_debug,
				      .no_color = global_no_color ||
						  app_cfg->no_color };
		shyake_ctx *ctx = shyake_init_ctx(&cfg);
		if (prompt_passphrase(ctx, config_dir) != 0) {
			shyake_free_ctx(ctx);
			free_app_config(app_cfg);
			free(config_dir);
			return EXIT_FAILURE;
		}
		shyake_err ret = shyake_block(ctx, target, is_unblock);
		if (ret == SHYAKE_OK)
			printf("%s %s.\n", target,
			       is_unblock ? "unblocked" : "blocked");
		else
			print_lib_error(ctx, is_unblock ? "Unblock failed." :
							  "Block failed.");
		shyake_free_ctx(ctx);
		free_app_config(app_cfg);
		free(config_dir);
		return ret == SHYAKE_OK ? EXIT_SUCCESS : EXIT_FAILURE;
	}

	if (strcmp(cmd, "blocklist") == 0) {
		const char *inst = app_cfg->instance;
		const char *user = app_cfg->username;

		if (!inst || !user) {
			fprintf(stderr,
				"Missing INSTANCE or USERNAME in config file.\n");
			free_app_config(app_cfg);
			free(config_dir);
			return EXIT_FAILURE;
		}
		shyake_config cfg = { .config_dir = config_dir,
				      .instance_url = inst,
				      .username = user,
				      .plain = global_plain,
				      .debug = global_debug,
				      .no_color = global_no_color ||
						  app_cfg->no_color };
		shyake_ctx *ctx = shyake_init_ctx(&cfg);
		if (prompt_passphrase(ctx, config_dir) != 0) {
			shyake_free_ctx(ctx);
			free_app_config(app_cfg);
			free(config_dir);
			return EXIT_FAILURE;
		}
		shyake_block_list *list = shyake_list_blocks(ctx);
		int ok = list != NULL;
		if (!list)
			print_lib_error(ctx, "Listing blocks failed.");
		shyake_free_ctx(ctx);
		if (list && list->count == 0) {
			printf("Block list is empty.\n");
		} else if (list) {
			for (int i = 0; i < list->count; i++) {
				char tbuf[64];
				cli_format_timestamp(list->entries[i].created,
						     app_cfg->tz_hours,
						     app_cfg->time_format, NULL,
						     tbuf, sizeof(tbuf));
				printf("%s  %s\n", tbuf,
				       list->entries[i].target);
			}
		}
		shyake_free_block_list(list);
		free_app_config(app_cfg);
		free(config_dir);
		return ok ? EXIT_SUCCESS : EXIT_FAILURE;
	}

	if (strcmp(cmd, "rotate") == 0) {
		const char *inst = app_cfg->instance;
		const char *user = app_cfg->username;

		if (!inst || !user) {
			fprintf(stderr,
				"Missing INSTANCE or USERNAME in config file.\n");
			free_app_config(app_cfg);
			free(config_dir);
			return EXIT_FAILURE;
		}
		shyake_config cfg = { .config_dir = config_dir,
				      .instance_url = inst,
				      .username = user,
				      .plain = global_plain,
				      .debug = global_debug,
				      .no_color = global_no_color ||
						  app_cfg->no_color };

		shyake_ctx *ctx = shyake_init_ctx(&cfg);
		if (prompt_passphrase(ctx, config_dir) != 0 ||
		    prompt_new_passphrase(ctx, config_dir) != 0) {
			shyake_free_ctx(ctx);
			free_app_config(app_cfg);
			free(config_dir);
			return EXIT_FAILURE;
		}
		fprintf(stderr, "Rotating keys for %s... ", user);
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
		free_app_config(app_cfg);
		free(config_dir);
		return ret == SHYAKE_OK ? EXIT_SUCCESS : EXIT_FAILURE;
	}

	if (strcmp(cmd, "fingerprint") == 0) {
		int do_update = 0;
		const char *target_user = NULL;
		for (int i = 2; i < argc; i++) {
			if (strcmp(argv[i], "--update") == 0) {
				do_update = 1;
			} else if (!target_user) {
				target_user = argv[i];
			}
		}

		const char *inst = app_cfg->instance;
		const char *user = app_cfg->username;

		if (!inst || !user) {
			fprintf(stderr,
				"Missing INSTANCE or USERNAME in config file.\n");
			free_app_config(app_cfg);
			free(config_dir);
			return EXIT_FAILURE;
		}
		shyake_config cfg = { .config_dir = config_dir,
				      .instance_url = inst,
				      .username = user,
				      .plain = global_plain,
				      .debug = global_debug,
				      .no_color = global_no_color ||
						  app_cfg->no_color };

		shyake_ctx *ctx = shyake_init_ctx(&cfg);
		int ret = 0;
		int is_self = (target_user == NULL);

		if (is_self)
			printf("Fingerprint for %s (local):\n", user);
		else if (!do_update)
			printf("Fetching public key for %s...\n", target_user);

		shyake_fp_result *fp =
			shyake_fingerprint(ctx, target_user, do_update);
		if (fp) {
			if (!is_self && do_update) {
				/* --update: only confirm, no art */
				printf("Updated known_hosts for %s.\n",
				       target_user);
			} else {
				cli_render_fingerprint(is_self ? user :
								 target_user,
						       fp, is_self);
			}
			shyake_free_fp_result(fp);
		} else {
			print_lib_error(ctx, "Fingerprint failed.");
			ret = -1;
		}

		shyake_free_ctx(ctx);
		free_app_config(app_cfg);
		free(config_dir);
		return ret == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
	}

	if (strcmp(cmd, "destroy") == 0) {
		const char *inst = app_cfg->instance;
		const char *user = app_cfg->username;

		printf("WARNING: This destroys your account on the instance "
		       "and deletes your\n");
		printf("local configuration and keys. All mail to and from you "
		       "is deleted.\n");
		printf("Your username stays locked on this instance and cannot "
		       "be registered\n");
		printf("again. Type your username to confirm: ");
		fflush(stdout);

		char buf[256];
		if (!fgets(buf, sizeof(buf), stdin)) {
			free_app_config(app_cfg);
			free(config_dir);
			return EXIT_FAILURE;
		}

		char *input = trim_whitespace(buf);
		if (strcmp(input, user) != 0) {
			fprintf(stderr, "Username mismatch. Aborted.\n");
			free_app_config(app_cfg);
			free(config_dir);
			return EXIT_FAILURE;
		}

		if (!inst || !user) {
			fprintf(stderr,
				"Missing INSTANCE or USERNAME in config file.\n");
			free_app_config(app_cfg);
			free(config_dir);
			return EXIT_FAILURE;
		}
		shyake_config cfg = { .config_dir = config_dir,
				      .instance_url = inst,
				      .username = user,
				      .plain = global_plain,
				      .debug = global_debug,
				      .no_color = global_no_color ||
						  app_cfg->no_color };

		shyake_ctx *ctx = shyake_init_ctx(&cfg);
		if (prompt_passphrase(ctx, config_dir) != 0) {
			shyake_free_ctx(ctx);
			free_app_config(app_cfg);
			free(config_dir);
			return EXIT_FAILURE;
		}
		shyake_err ret = shyake_destroy(ctx);

		if (ret == SHYAKE_OK) {
			char cmd_buf[512];
			snprintf(cmd_buf, sizeof(cmd_buf), "rm -rf %s/*",
				 config_dir);
			system(cmd_buf);
			printf("Account destroyed. "
			       "Local configuration and keys deleted.\n");
		} else {
			print_lib_error(ctx, "Destroy failed.");
		}

		shyake_free_ctx(ctx);
		free_app_config(app_cfg);

		free(config_dir);
		return ret == SHYAKE_OK ? EXIT_SUCCESS : EXIT_FAILURE;
	}

	if (strcmp(cmd, "save") == 0) {
		if (argc < 3) {
			fprintf(stderr, "Usage: shyake save <id>\n");
			free_app_config(app_cfg);
			free(config_dir);
			return EXIT_FAILURE;
		}
		const char *mail_id = argv[2];
		const char *inst = app_cfg->instance;
		const char *user = app_cfg->username;

		if (!inst || !user) {
			fprintf(stderr,
				"Missing INSTANCE or USERNAME in config file.\n");
			free_app_config(app_cfg);
			free(config_dir);
			return EXIT_FAILURE;
		}
		shyake_config cfg = { .config_dir = config_dir,
				      .instance_url = inst,
				      .username = user,
				      .plain = global_plain,
				      .debug = global_debug,
				      .no_color = global_no_color ||
						  app_cfg->no_color };
		shyake_ctx *ctx = shyake_init_ctx(&cfg);
		if (prompt_passphrase(ctx, config_dir) != 0) {
			shyake_free_ctx(ctx);
			free_app_config(app_cfg);
			free(config_dir);
			return EXIT_FAILURE;
		}
		shyake_err ret = shyake_save_mail(ctx, mail_id);
		if (ret == SHYAKE_OK)
			printf("Mail saved.\n");
		else
			print_lib_error(ctx, "Save failed.");
		shyake_free_ctx(ctx);
		free_app_config(app_cfg);
		free(config_dir);
		return ret == SHYAKE_OK ? EXIT_SUCCESS : EXIT_FAILURE;
	}

	if (strcmp(cmd, "read") == 0) {
		int raw = 0;
		const char *mail_id = NULL;

		static struct option read_options[] = {
			{ "raw", no_argument, 0, 'r' }, { 0, 0, 0, 0 }
		};
		int opt, opt_idx = 0;
		optind = 2;
		while ((opt = getopt_long(argc, argv, "r", read_options,
					  &opt_idx)) != -1) {
			switch (opt) {
			case 'r':
				raw = 1;
				break;
			default:
				break;
			}
		}
		if (optind < argc)
			mail_id = argv[optind];

		/* read drafts <id> — decrypt and display a draft in full */
		if (mail_id && strcmp(mail_id, "drafts") == 0) {
			const char *draft_id =
				(optind + 1 < argc) ? argv[optind + 1] : NULL;
			if (!draft_id) {
				fprintf(stderr,
					"Usage: shyake read drafts <id>\n");
				free_app_config(app_cfg);
				free(config_dir);
				return EXIT_FAILURE;
			}
			shyake_config dcfg = {
				.config_dir = config_dir,
				.instance_url = app_cfg->instance ?
							app_cfg->instance :
							"",
				.username = app_cfg->username ?
						    app_cfg->username :
						    "",
				.plain = global_plain,
				.debug = global_debug,
				.no_color = global_no_color || app_cfg->no_color
			};
			shyake_ctx *dctx = shyake_init_ctx(&dcfg);
			if (prompt_passphrase(dctx, config_dir) != 0) {
				shyake_free_ctx(dctx);
				free_app_config(app_cfg);
				free(config_dir);
				return EXIT_FAILURE;
			}
			int dret = 0;
			shyake_mail_detail *d = cli_read_draft(
				dctx, config_dir, app_cfg->username, draft_id);
			if (d) {
				char *re = raw ? NULL : describe_reply(dctx, d);
				cli_render_mail_detail(
					d, re, raw, dcfg.no_color, dcfg.plain,
					app_cfg->tz_hours, app_cfg->time_format,
					app_cfg->time_format_recent);
				free(re);
				shyake_free_mail_detail(d);
			} else {
				print_drafts_error("Draft read failed.");
				dret = -1;
			}
			shyake_free_ctx(dctx);
			free_app_config(app_cfg);
			free(config_dir);
			return dret == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
		}

		if (!mail_id) {
			fprintf(stderr, "Usage: shyake read <id>\n");
			free_app_config(app_cfg);
			free(config_dir);
			return EXIT_FAILURE;
		}

		const char *user = app_cfg->username;
		if (!user) {
			fprintf(stderr, "Missing USERNAME in config file.\n");
			free_app_config(app_cfg);
			free(config_dir);
			return EXIT_FAILURE;
		}
		shyake_config cfg = {
			.config_dir = config_dir,
			.instance_url = app_cfg->instance ? app_cfg->instance :
							    "",
			.username = user,
			.plain = global_plain,
			.debug = global_debug,
			.no_color = global_no_color || app_cfg->no_color
		};
		shyake_ctx *ctx = shyake_init_ctx(&cfg);
		if (prompt_passphrase(ctx, config_dir) != 0) {
			shyake_free_ctx(ctx);
			free_app_config(app_cfg);
			free(config_dir);
			return EXIT_FAILURE;
		}
		int ret = 0;
		shyake_mail_detail *d = shyake_read_saved(ctx, mail_id);
		if (d) {
			char *re = raw ? NULL : describe_reply(ctx, d);
			cli_render_mail_detail(d, re, raw, cfg.no_color,
					       cfg.plain, app_cfg->tz_hours,
					       app_cfg->time_format,
					       app_cfg->time_format_recent);
			free(re);
			shyake_free_mail_detail(d);
		} else {
			print_lib_error(ctx, "Read failed.");
			ret = -1;
		}
		shyake_free_ctx(ctx);
		free_app_config(app_cfg);
		free(config_dir);
		return ret == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
	}

	if (strcmp(cmd, "enc") == 0 || strcmp(cmd, "encrypt") == 0) {
		char *recipient = NULL;
		char *out_path = NULL;
		const char *in_path = NULL;

		static struct option enc_options[] = {
			{ "to", required_argument, 0, 't' },
			{ "output", required_argument, 0, 'o' },
			{ 0, 0, 0, 0 }
		};
		int opt, opt_idx = 0;
		optind = 2;
		while ((opt = getopt_long(argc, argv, "t:o:", enc_options,
					  &opt_idx)) != -1) {
			switch (opt) {
			case 't':
				recipient = optarg;
				break;
			case 'o':
				out_path = optarg;
				break;
			default:
				break;
			}
		}
		if (optind < argc)
			in_path = argv[optind];

		if (!in_path) {
			fprintf(stderr,
				"Usage: shyake enc <file> [-t <username>] "
				"[-o <output>]\n");
			free_app_config(app_cfg);
			free(config_dir);
			return EXIT_FAILURE;
		}

		const char *inst = app_cfg->instance;
		const char *user = app_cfg->username;

		/* instance/user only required when fetching recipient pubkey */
		if (recipient && (!inst || !user)) {
			fprintf(stderr,
				"Missing INSTANCE or USERNAME in config file.\n");
			free_app_config(app_cfg);
			free(config_dir);
			return EXIT_FAILURE;
		}
		shyake_config cfg = { .config_dir = config_dir,
				      .instance_url = inst ? inst : "",
				      .username = user ? user : "",
				      .plain = global_plain,
				      .debug = global_debug,
				      .no_color = global_no_color ||
						  app_cfg->no_color };
		shyake_ctx *ctx = shyake_init_ctx(&cfg);
		char *used_path = NULL;
		shyake_err ret = shyake_enc_file(ctx, in_path, out_path,
						 recipient, &used_path);
		if (ret == SHYAKE_OK)
			fprintf(stderr, "Encrypted: %s\n", used_path);
		else
			print_lib_error(ctx, "Encryption failed.");
		free(used_path);
		shyake_free_ctx(ctx);
		free_app_config(app_cfg);
		free(config_dir);
		return ret == SHYAKE_OK ? EXIT_SUCCESS : EXIT_FAILURE;
	}

	if (strcmp(cmd, "dec") == 0 || strcmp(cmd, "decrypt") == 0) {
		char *out_path = NULL;
		const char *in_path = NULL;

		static struct option dec_options[] = {
			{ "output", required_argument, 0, 'o' }, { 0, 0, 0, 0 }
		};
		int opt, opt_idx = 0;
		optind = 2;
		while ((opt = getopt_long(argc, argv, "o:", dec_options,
					  &opt_idx)) != -1) {
			switch (opt) {
			case 'o':
				out_path = optarg;
				break;
			default:
				break;
			}
		}
		if (optind < argc)
			in_path = argv[optind];

		if (!in_path) {
			fprintf(stderr,
				"Usage: shyake dec <file> [-o <output>]\n");
			free_app_config(app_cfg);
			free(config_dir);
			return EXIT_FAILURE;
		}

		const char *user = app_cfg->username;
		shyake_config cfg = {
			.config_dir = config_dir,
			.instance_url = app_cfg->instance ? app_cfg->instance :
							    "",
			.username = user ? user : "",
			.plain = global_plain,
			.debug = global_debug,
			.no_color = global_no_color || app_cfg->no_color
		};
		shyake_ctx *ctx = shyake_init_ctx(&cfg);
		if (prompt_passphrase(ctx, config_dir) != 0) {
			shyake_free_ctx(ctx);
			free_app_config(app_cfg);
			free(config_dir);
			return EXIT_FAILURE;
		}
		shyake_err ret = shyake_dec_file(ctx, in_path, out_path);
		if (ret == SHYAKE_OK) {
			if (out_path)
				fprintf(stderr, "Decrypted: %s\n", out_path);
		} else {
			print_lib_error(ctx, "Decryption failed.");
		}
		shyake_free_ctx(ctx);
		free_app_config(app_cfg);
		free(config_dir);
		return ret == SHYAKE_OK ? EXIT_SUCCESS : EXIT_FAILURE;
	}

	if (strcmp(cmd, "update") == 0) {
		char *version_url = build_version_url(app_cfg->instance);
		if (!version_url) {
			free_app_config(app_cfg);
			free(config_dir);
			return EXIT_FAILURE;
		}

		/* update (no args) — show version info */
		if (argc < 3) {
			cli_version_info *info = cli_get_latest_version(
				version_url, global_debug);
			free(version_url);
			free_app_config(app_cfg);
			free(config_dir);

			if (!info) {
				print_failure("Update failed.",
					      "Cannot get the latest version.");
				return EXIT_FAILURE;
			}

			int show_preview = info->pre_release &&
					   cli_version_cmp(info->pre_release,
							   info->release) > 0;

			printf("Installed: %s\n", SHYAKE_VERSION);
			printf("Stable:    %s\n",
			       info->release ? info->release : "N/A");
			if (show_preview)
				printf("Preview:   %s\n", info->pre_release);

			int on_stable = info->release &&
					strcmp(SHYAKE_VERSION, info->release) ==
						0;
			int on_preview =
				show_preview && info->pre_release &&
				strcmp(SHYAKE_VERSION, info->pre_release) == 0;
			int want_stable = !on_stable;
			int want_preview = show_preview && !on_preview;

			if (want_stable || want_preview) {
				printf("\n");
				if (want_stable && want_preview)
					printf("Run 'shyake update stable' or "
					       "'shyake update preview' to update.\n");
				else if (want_stable)
					printf("Run 'shyake update stable' to update.\n");
				else
					printf("Run 'shyake update preview' to install "
					       "the preview release.\n");
			}

			cli_free_version_info(info);
			return EXIT_SUCCESS;
		}

		/* update stable | update preview */
		const char *subcmd = argv[2];
		cli_update_channel channel;
		if (strcmp(subcmd, "stable") == 0) {
			channel = CLI_UPDATE_STABLE;
		} else if (strcmp(subcmd, "preview") == 0) {
			channel = CLI_UPDATE_PREVIEW;
		} else {
			fprintf(stderr,
				"Usage: shyake update [stable|preview]\n");
			free(version_url);
			free_app_config(app_cfg);
			free(config_dir);
			return EXIT_FAILURE;
		}

		int ret = cli_self_update(version_url, SHYAKE_VERSION, channel,
					  global_debug);
		free(version_url);
		free_app_config(app_cfg);
		free(config_dir);
		return ret == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
	}

	fprintf(stderr, "Unknown command: %s\n", cmd);
	free_app_config(app_cfg);
	free(config_dir);
	return EXIT_FAILURE;
}
