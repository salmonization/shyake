#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <openssl/sha.h>
#include "display.h"

/* ------------------------------------------------------------------ */
/* UTF-8 display width helpers                                        */
/* ------------------------------------------------------------------ */

/* decode one UTF-8 codepoint; advance *s; return codepoint or -1 */
static int utf8_next(const char **s)
{
	const unsigned char *p = (const unsigned char *)*s;
	if (!*p)
		return -1;
	int cp, n;
	if (*p < 0x80) {
		cp = *p;
		n = 1;
	} else if (*p < 0xC2) {
		(*s)++;
		return 0;
	} /* continuation / invalid */
	else if (*p < 0xE0) {
		cp = *p & 0x1F;
		n = 2;
	} else if (*p < 0xF0) {
		cp = *p & 0x0F;
		n = 3;
	} else if (*p < 0xF8) {
		cp = *p & 0x07;
		n = 4;
	} else {
		(*s)++;
		return 0;
	}
	for (int i = 1; i < n; i++) {
		if ((p[i] & 0xC0) != 0x80) {
			(*s)++;
			return 0;
		}
		cp = (cp << 6) | (p[i] & 0x3F);
	}
	*s += n;
	return cp;
}

/* return terminal display width of a single Unicode codepoint */
static int cp_width(int cp)
{
	if (cp <= 0)
		return 0;
	/* CJK Unified Ideographs and common wide blocks */
	if ((cp >= 0x1100 && cp <= 0x115F) || /* Hangul Jamo */
	    (cp == 0x2329 || cp == 0x232A) ||
	    (cp >= 0x2E80 && cp <= 0x303E) || /* CJK Radicals, etc. */
	    (cp >= 0x3040 && cp <= 0x33FF) || /* Hiragana..CJK Compat */
	    (cp >= 0x3400 && cp <= 0x4DBF) || /* CJK Ext-A */
	    (cp >= 0x4E00 && cp <= 0xA4CF) || /* CJK Unified */
	    (cp >= 0xA960 && cp <= 0xA97F) || /* Hangul Jamo Ext-A */
	    (cp >= 0xAC00 && cp <= 0xD7FF) || /* Hangul Syllables */
	    (cp >= 0xF900 && cp <= 0xFAFF) || /* CJK Compat Ideographs */
	    (cp >= 0xFE10 && cp <= 0xFE1F) || /* Vertical forms */
	    (cp >= 0xFE30 && cp <= 0xFE6F) || /* CJK Compat Forms */
	    (cp >= 0xFF00 && cp <= 0xFF60) || /* Fullwidth */
	    (cp >= 0xFFE0 && cp <= 0xFFE6) ||
	    (cp >= 0x1B000 && cp <= 0x1B0FF) || /* Kana Supplement */
	    (cp >= 0x1F004 && cp <= 0x1F0CF) || /* Mahjong/Playing cards */
	    (cp >= 0x1F300 && cp <= 0x1F9FF) || /* Misc symbols & Emoji */
	    (cp >= 0x20000 && cp <= 0x2FFFD) || /* CJK Ext-B..F */
	    (cp >= 0x30000 && cp <= 0x3FFFD))
		return 2;
	return 1;
}

/* return display width of a UTF-8 string */
int cli_utf8_width(const char *s)
{
	int w = 0;
	while (*s) {
		int cp = utf8_next(&s);
		if (cp > 0)
			w += cp_width(cp);
	}
	return w;
}

/*
 * Truncate UTF-8 string so its display width <= max_w.
 * Writes into buf (size buf_sz).  Appends ellipsis (".." or "..."
 * depending on available space) when truncation actually occurs.
 * Returns the display width of the result written.
 */
int cli_utf8_truncate(const char *src, char *buf, int buf_sz, int max_w)
{
	const char *ellipsis = "...";
	int ew = 3;
	if (max_w < 3) {
		buf[0] = '\0';
		return 0;
	}

	int total_w = cli_utf8_width(src);
	if (total_w <= max_w) {
		int len = (int)strlen(src);
		if (len >= buf_sz)
			len = buf_sz - 1;
		memcpy(buf, src, len);
		buf[len] = '\0';
		return total_w;
	}

	/* scan forward, accumulate width up to (max_w - ew) */
	int limit_w = max_w - ew;
	int accum = 0;
	const char *p = src;
	const char *last_safe = src;
	while (*p) {
		int cp = utf8_next(&p);
		int cw = cp > 0 ? cp_width(cp) : 0;
		if (accum + cw > limit_w)
			break;
		accum += cw;
		last_safe = p;
	}
	int head = (int)(last_safe - src);
	int elen = (int)strlen(ellipsis);
	if (head + elen >= buf_sz)
		head = buf_sz - elen - 1;
	if (head < 0)
		head = 0;
	memcpy(buf, src, head);
	memcpy(buf + head, ellipsis, elen);
	buf[head + elen] = '\0';
	return accum + ew;
}

/* ------------------------------------------------------------------ */
/* Pager                                                              */
/* ------------------------------------------------------------------ */

static int pager_pid = -1;
static int saved_stdout = -1;

void cli_setup_pager(int disable_pager)
{
	/* spawn less pager if stdout is TTY */
	if (disable_pager || !isatty(STDOUT_FILENO))
		return;

	int fd[2];
	if (pipe(fd) < 0)
		return;

	pager_pid = fork();
	if (pager_pid < 0) {
		close(fd[0]);
		close(fd[1]);
		return;
	}
	if (pager_pid == 0) {
		dup2(fd[0], STDIN_FILENO);
		close(fd[0]);
		close(fd[1]);
		execlp("less", "less", "-F", "-R", "-X", NULL);
		exit(1);
	} else {
		saved_stdout = dup(STDOUT_FILENO);
		dup2(fd[1], STDOUT_FILENO);
		close(fd[0]);
		close(fd[1]);
	}
}

int cli_pager_active(void)
{
	return saved_stdout >= 0;
}

void cli_wait_pager(void)
{
	/* wait for less pager process */
	if (pager_pid > 0) {
		fflush(stdout);
		close(STDOUT_FILENO);
		if (saved_stdout >= 0) {
			dup2(saved_stdout, STDOUT_FILENO);
			close(saved_stdout);
			saved_stdout = -1;
		}
		waitpid(pager_pid, NULL, 0);
		pager_pid = -1;
	}
}

/* ------------------------------------------------------------------ */
/* Terminal width                                                     */
/* ------------------------------------------------------------------ */

int cli_get_terminal_width(void)
{
	/* get current terminal width */
	struct winsize w;
	if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == 0 && w.ws_col > 0)
		return w.ws_col;
	return 80;
}

/* ------------------------------------------------------------------ */
/* Word wrap                                                          */
/* ------------------------------------------------------------------ */

void cli_print_word_wrap(const char *text, int indent, int width)
{
	/* word-wrap text with UTF-8 display width awareness */
	int avail = width - indent;
	if (avail < 20)
		avail = 20;
	int first = 1;
	const char *p = text;
	while (*p) {
		if (!first)
			printf("\n%*s", indent, "");
		first = 0;
		const char *line_start = p;
		const char *last_space = NULL;
		const char *safe_cut = p;
		int accum = 0;
		while (*p) {
			const char *before = p;
			int cp = utf8_next(&p);
			if (cp == ' ') {
				last_space = before;
			}
			int cw = cp > 0 ? cp_width(cp) : 0;
			if (accum + cw > avail) {
				p = before; /* rewind to just before this codepoint */
				break;
			}
			accum += cw;
			safe_cut = p;
		}
		if (*p == '\0') {
			printf("%.*s", (int)(safe_cut - line_start),
			       line_start);
			break;
		}
		if (last_space != NULL && last_space > line_start) {
			printf("%.*s", (int)(last_space - line_start),
			       line_start);
			p = last_space;
			while (*p == ' ')
				p++;
		} else {
			printf("%.*s", (int)(safe_cut - line_start),
			       line_start);
			p = safe_cut;
		}
	}
	printf("\n");
}

/* ------------------------------------------------------------------ */
/* Timestamp formatting with timezone support                         */
/* ------------------------------------------------------------------ */

void cli_format_timestamp(i64 ts, int tz_hours, const char *fmt,
			  const char *fmt_recent, char *buf, int buf_len)
{
	/* convert UNIX ts to broken-down time with optional tz offset */
	const char *f = fmt ? fmt : "%Y-%m-%d %H:%M";
	time_t t = (time_t)ts;
	struct tm tmbuf;
	struct tm *tmi;
	if (tz_hours == TZ_AUTO) {
		tmi = localtime_r(&t, &tmbuf);
	} else {
		/* apply manual UTC offset */
		time_t shifted = t + (time_t)tz_hours * 3600;
		tmi = gmtime_r(&shifted, &tmbuf);
	}
	if (!tmi) {
		snprintf(buf, buf_len, "?");
		return;
	}
	if (fmt_recent) {
		time_t now = time(NULL);
		if (now - t < 180 * 24 * 3600)
			f = fmt_recent;
	}
	strftime(buf, buf_len, f, tmi);
}

/* ------------------------------------------------------------------ */
/* Party / size display helpers                                        */
/* ------------------------------------------------------------------ */

void cli_format_party(const char *party, char *buf, int buf_len)
{
	/* for remote parties keep username@ only */
	const char *at = strchr(party, '@');
	if (at) {
		int name_len = (int)(at - party);
		snprintf(buf, buf_len, "%.*s@", name_len, party);
	} else {
		snprintf(buf, buf_len, "%s", party);
	}
}

void cli_format_size(int size, char *buf, int buf_len)
{
	if (size >= 1024)
		snprintf(buf, buf_len, "%dK", size / 1024);
	else
		snprintf(buf, buf_len, "%d", size);
}

/* ------------------------------------------------------------------ */
/* Fingerprint display                                                */
/* ------------------------------------------------------------------ */

void cli_print_fingerprint_hex(const unsigned char *fp)
{
	for (int i = 0; i < 16; i++) {
		printf("%02X%02X", fp[i * 2], fp[i * 2 + 1]);
		if (i == 7)
			printf("\n");
		else if (i == 15)
			printf("\n");
		else if (i == 3 || i == 11)
			printf("  ");
		else
			printf(" ");
	}
}

void cli_print_randomart(const unsigned char *fp)
{
	int x = 8, y = 4;
	int grid[17][9] = { 0 };

	for (int i = 0; i < SHA256_DIGEST_LENGTH; i++) {
		unsigned char b = fp[i];
		for (int j = 0; j < 4; j++) {
			int dir = (b >> (j * 2)) & 3;
			int dx = (dir & 1) ? 1 : -1;
			int dy = (dir & 2) ? 1 : -1;
			x += dx;
			y += dy;
			if (x < 0)
				x = 0;
			if (x > 16)
				x = 16;
			if (y < 0)
				y = 0;
			if (y > 8)
				y = 8;
			grid[x][y]++;
		}
	}

	const char *chars = " .o+=*BOX@%&#/^";
	printf("+-----------------+\n");
	for (int j = 0; j < 9; j++) {
		printf("|");
		for (int i = 0; i < 17; i++) {
			if (i == 8 && j == 4) {
				printf("S");
			} else if (i == x && j == y) {
				printf("E");
			} else {
				int val = grid[i][j];
				if (val > 14)
					val = 14;
				printf("%c", chars[val]);
			}
		}
		printf("|\n");
	}
	printf("+-----------------+\n");
}

void cli_render_fingerprint(const char *label, const shyake_fp_result *r,
			    int is_self)
{
	(void)label;
	if (is_self) {
		cli_print_fingerprint_hex(r->remote_fp);
		cli_print_randomart(r->remote_fp);
		return;
	}

	if (r->has_local) {
		printf("\n[Local known_hosts]\n");
		cli_print_fingerprint_hex(r->local_fp);
		cli_print_randomart(r->local_fp);

		printf("\n[Remote server]\n");
		cli_print_fingerprint_hex(r->remote_fp);
		cli_print_randomart(r->remote_fp);

		if (r->match)
			printf("\nStatus: MATCH\n");
		else
			printf("\nStatus: MISMATCH (WARNING: Key has changed)\n\n"
			       "Please verify the new fingerprint in a secondary, "
			       "trusted out-of-band channel\n"
			       "before running the update command "
			       "('shyake fingerprint <username> --update').\n");
	} else {
		printf("\n[Remote server]\n");
		cli_print_fingerprint_hex(r->remote_fp);
		cli_print_randomart(r->remote_fp);
		printf("\nStatus: NEW HOST (No local key found)\n");
	}
}
