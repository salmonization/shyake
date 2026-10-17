#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include "shyake.h"
#include "internal.h"
#include "display.h"
#include "config.h"
#include "compose.h"

/* whole stream, read to its end */
u8 *cli_read_all(FILE *f, usize *out_len)
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

int cli_parse_compose(char *buf, char **to, char **subject, char **body)
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
		char *trimmed = cli_trim(line);
		if (strcmp(trimmed, "---") == 0) {
			*body = next ? next : line + strlen(line);
			return 0;
		}
		if (strncmp(trimmed, "To:", 3) == 0)
			*to = cli_trim(trimmed + 3);
		else if (strncmp(trimmed, "Subject:", 8) == 0)
			*subject = cli_trim(trimmed + 8);
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
	char *buf = (char *)cli_read_all(f, &len);
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

char *cli_format_original(const shyake_mail_detail *d, const app_config *cfg)
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
char *cli_with_scissors(const char *initial, const char *orig)
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
const char *cli_after_scissors(const char *buf)
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
	const char *start = cli_after_scissors(text);
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

char *cli_edit_draft(const app_config *cfg, const char *config_dir,
		     const char *initial, const char *orig)
{
	const char *editor = resolve_editor(cfg);
	int split = orig && is_vim(editor);
	char *full = orig && !split ? cli_with_scissors(initial, orig) :
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
		const char *rest = cli_after_scissors(buf);
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
