/* Thread views: the lane graph, the list, the mails in full */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "display.h"

/* ------------------------------------------------------------------ */
/* Thread graph (check thread, fetch thread, read thread)             */
/* ------------------------------------------------------------------ */

/*
 * Lanes, oldest mail first, like git log --graph:
 *
 *   *        a mail; @ is the mail asked for
 *   |\       the mail above has a later reply in a new lane
 *   | *      a reply in the new lane
 *   * |      a reply in the lane of the mail it answers
 *
 * A mail takes the lane of the mail it answers when it is the last
 * reply to it, else it opens the lane to the right. A lane ends with
 * a mail that has no replies.
 */
typedef struct {
	char **fork; /* row before entry i, NULL if none */
	char **node; /* row of entry i */
	char **below; /* lanes under entry i, for its header and body */
	int width; /* widest row */
} thread_graph;

static void free_graph(thread_graph *g, int count)
{
	for (int i = 0; i < count; i++) {
		free(g->fork[i]);
		free(g->node[i]);
		free(g->below[i]);
	}
	free(g->fork);
	free(g->node);
	free(g->below);
}

/* row text: one column per lane, a space between, no trailing space */
static char *lane_row(const int *lanes, int nl, int mark_lane, char mark)
{
	char *row = malloc((usize)(nl > 0 ? nl * 2 : 1));
	int len = 0;
	for (int i = 0; i < nl; i++) {
		if (i > 0)
			row[len++] = ' ';
		row[len++] = i == mark_lane ? mark : lanes[i] >= 0 ? '|' : ' ';
	}
	while (len > 0 && row[len - 1] == ' ')
		len--;
	row[len] = '\0';
	return row;
}

static void build_graph(const shyake_thread *t, thread_graph *g)
{
	int n = t->count;
	g->fork = calloc((usize)n, sizeof(char *));
	g->node = calloc((usize)n, sizeof(char *));
	g->below = calloc((usize)n, sizeof(char *));
	g->width = 0;
	int *left = calloc((usize)n, sizeof(int)); /* replies not drawn */
	int *lanes = malloc((usize)(n + 1) * sizeof(int));
	int nl = 0;
	for (int i = 0; i < n; i++)
		if (t->entries[i].parent >= 0)
			left[t->entries[i].parent]++;
	int *kids = malloc((usize)n * sizeof(int));
	memcpy(kids, left, (usize)n * sizeof(int));

	for (int i = 0; i < n; i++) {
		int p = t->entries[i].parent, lane = 0;
		int at = -1;
		for (int k = 0; k < nl && p >= 0; k++)
			if (lanes[k] == p)
				at = k;
		if (at < 0) {
			/* the first mail: the first free lane */
			while (lane < nl && lanes[lane] >= 0)
				lane++;
		} else if (left[p] == 1) {
			lane = at;
		} else {
			/* a new lane right of the parent; the lanes up to
			 * the next free one move right */
			int j = at + 1;
			while (j < nl && lanes[j] >= 0)
				j++;
			int w = (j + 1 > nl ? j + 1 : nl) * 2;
			char *f = malloc((usize)w + 1);
			memset(f, ' ', (usize)w);
			for (int k = 0; k < nl; k++)
				if (lanes[k] >= 0 && (k <= at || k > j))
					f[2 * k] = '|';
			f[2 * at + 1] = '\\';
			for (int k = at + 1; k < j; k++)
				f[2 * k + 1] = '\\';
			int len = w;
			while (len > 0 && f[len - 1] == ' ')
				len--;
			f[len] = '\0';
			g->fork[i] = f;
			if (j == nl)
				nl++;
			for (int k = j; k > at + 1; k--)
				lanes[k] = lanes[k - 1];
			lane = at + 1;
		}
		if (p >= 0)
			left[p]--;
		if (lane == nl)
			nl++;
		lanes[lane] = i;
		g->node[i] =
			lane_row(lanes, nl, lane, i == t->focus ? '@' : '*');
		if (kids[i] == 0)
			lanes[lane] = -1;
		while (nl > 0 && lanes[nl - 1] < 0)
			nl--;
		g->below[i] = lane_row(lanes, nl, -1, ' ');
	}
	for (int i = 0; i < n; i++) {
		int w = (int)strlen(g->node[i]);
		if (g->fork[i] && (int)strlen(g->fork[i]) > w)
			w = (int)strlen(g->fork[i]);
		if (w > g->width)
			g->width = w;
	}
	free(left);
	free(kids);
	free(lanes);
}

/* graph cell padded to the graph width; @ in color; width < 0 ends
 * the line after the graph */
static void print_graph(const char *row, int width, int no_color)
{
	for (const char *c = row; *c; c++) {
		if (*c == '@' && !no_color)
			printf("\033[1;33m@\033[0m");
		else
			putchar(*c);
	}
	if (width < 0)
		printf("\n");
	else
		printf("%*s", width - (int)strlen(row) + 2, "");
}

/* check thread --json: entries in display order */
static void print_thread_json(const shyake_thread *t,
			      const cli_render_opts *opts)
{
	cJSON *arr = cJSON_CreateArray();
	for (int i = 0; i < t->count; i++) {
		const shyake_thread_entry *e = &t->entries[i];
		cJSON *o = cJSON_CreateObject();
		if (e->mail_id) {
			char dbuf[64], sbuf[16];
			cli_format_timestamp(e->timestamp, opts->tz_hours,
					     opts->time_fmt,
					     opts->time_fmt_recent, dbuf,
					     sizeof(dbuf));
			cli_format_size(e->size, sbuf, sizeof(sbuf));
			cJSON_AddStringToObject(o, "mail_id", e->mail_id);
			cJSON_AddStringToObject(o, "sender", e->sender);
			cJSON_AddStringToObject(o, "recipient", e->recipient);
			cJSON_AddStringToObject(o, "subject",
						e->subject ?
							e->subject :
							"<decryption failed>");
			cJSON_AddStringToObject(o, "size", sbuf);
			cJSON_AddStringToObject(o, "date", dbuf);
			cJSON_AddBoolToObject(o, "saved",
					      e->where & SHYAKE_IN_SAVED);
		} else {
			cJSON_AddNullToObject(o, "mail_id");
			cJSON_AddTrueToObject(o, "missing");
		}
		/* parent: array index, also when it is missing */
		const char *pid =
			e->parent >= 0 ? t->entries[e->parent].mail_id : NULL;
		if (pid)
			cJSON_AddStringToObject(o, "reply_to", pid);
		else
			cJSON_AddNullToObject(o, "reply_to");
		if (e->parent >= 0)
			cJSON_AddNumberToObject(o, "parent", e->parent);
		else
			cJSON_AddNullToObject(o, "parent");
		cJSON_AddItemToArray(arr, o);
	}
	char *out = cJSON_PrintUnformatted(arr);
	printf("%s\n", out);
	free(out);
	cJSON_Delete(arr);
}

void cli_render_thread_list(const shyake_thread *t, const cli_render_opts *opts)
{
	if (!t || t->count == 0)
		return;
	int n = t->count;
	if (opts->json_out) {
		print_thread_json(t, opts);
		return;
	}

	thread_graph g;
	build_graph(t, &g);

	/* cells: ID, From, Subject, Size, Date */
	char **id = calloc((usize)n, sizeof(char *));
	char **from = calloc((usize)n, sizeof(char *));
	char **size = calloc((usize)n, sizeof(char *));
	char **date = calloc((usize)n, sizeof(char *));
	int w_id = 7, w_from = 4, w_sub = 7, w_sz = 4;
	for (int i = 0; i < n; i++) {
		const shyake_thread_entry *e = &t->entries[i];
		char buf[256];
		if (!e->mail_id)
			snprintf(buf, sizeof(buf), "(missing)");
		else if (e->where == SHYAKE_IN_SAVED && !opts->local)
			snprintf(buf, sizeof(buf), "%s (saved)", e->mail_id);
		else
			snprintf(buf, sizeof(buf), "%s", e->mail_id);
		id[i] = strdup(buf);
		buf[0] = '\0';
		if (e->sender)
			cli_format_party(e->sender, buf, sizeof(buf));
		from[i] = strdup(buf);
		buf[0] = '\0';
		if (e->mail_id)
			cli_format_size(e->size, buf, sizeof(buf));
		size[i] = strdup(buf);
		buf[0] = '\0';
		if (e->mail_id)
			cli_format_timestamp(e->timestamp, opts->tz_hours,
					     opts->time_fmt,
					     opts->time_fmt_recent, buf,
					     sizeof(buf));
		date[i] = strdup(buf);
		int l = (int)strlen(id[i]);
		w_id = l > w_id ? l : w_id;
		l = (int)strlen(from[i]);
		w_from = l > w_from ? l : w_from;
		l = e->subject ? cli_utf8_width(e->subject) : 0;
		w_sub = l > w_sub ? l : w_sub;
		l = (int)strlen(size[i]);
		w_sz = l > w_sz ? l : w_sz;
	}
	if (!opts->plain) {
		int tw = opts->term_width > 0 ? opts->term_width :
						cli_get_terminal_width();
		int max_sub = tw - (g.width + 2) - (w_id + 1) - (w_from + 1) -
			      (w_sz + 1) - 13;
		if (max_sub < 15)
			max_sub = 15;
		if (w_sub > max_sub)
			w_sub = max_sub;
	}

	cli_setup_pager(opts->plain);
	const char *ul_on = opts->no_color ? "" : "\033[4m";
	const char *ul_off = opts->no_color ? "" : "\033[24m";
	const char *c_cy = opts->no_color ? "" : "\033[36m";
	const char *c_mg = opts->no_color ? "" : "\033[35m";
	const char *c_rs = opts->no_color ? "" : "\033[0m";

	if (!opts->no_header) {
		printf("%*s", g.width + 2, "");
		printf("%sMail ID%s%*s %sFrom%s%*s %sSubject%s%*s %sSize%s%*s "
		       "%sDate%s\n",
		       ul_on, ul_off, w_id - 7, "", ul_on, ul_off, w_from - 4,
		       "", ul_on, ul_off, w_sub - 7, "", ul_on, ul_off,
		       w_sz - 4, "", ul_on, ul_off);
	}
	for (int i = 0; i < n; i++) {
		const shyake_thread_entry *e = &t->entries[i];
		if (g.fork[i]) {
			print_graph(g.fork[i], -1, opts->no_color);
		}
		print_graph(g.node[i], g.width, opts->no_color);
		if (!e->mail_id) {
			printf("%s\n", id[i]);
			continue;
		}
		char sub[512];
		int dw = cli_utf8_truncate(e->subject ? e->subject :
							"<decryption failed>",
					   sub, sizeof(sub), w_sub);
		printf("%-*s %s%-*s%s %s%*s %s%-*s%s %s\n", w_id, id[i], c_cy,
		       w_from, from[i], c_rs, sub, w_sub - dw, "", c_mg, w_sz,
		       size[i], c_rs, date[i]);
	}
	if (!opts->no_header)
		printf("\nTotal: %d item%s\n", n, n == 1 ? "" : "s");
	cli_wait_pager();

	for (int i = 0; i < n; i++) {
		free(id[i]);
		free(from[i]);
		free(size[i]);
		free(date[i]);
	}
	free(id);
	free(from);
	free(size);
	free(date);
	free_graph(&g, n);
}

/* body lines, each after the graph prefix */
static void print_body(const char *prefix, int width, const char *body,
		       int no_color)
{
	const char *s = body;
	while (*s) {
		const char *nl = strchr(s, '\n');
		usize len = nl ? (usize)(nl - s) : strlen(s);
		if (prefix)
			print_graph(prefix, width, no_color);
		printf("%.*s\n", (int)len, s);
		if (!nl)
			break;
		s = nl + 1;
	}
}

/* how cli_render_thread_mails draws */
struct mails_view {
	const thread_graph *g; /* NULL when flat */
	int no_color;
	int tz_hours;
	const char *time_fmt;
	const char *time_fmt_recent;
};

/* header lines and body of entry i; d is NULL when unavailable */
static void print_thread_mail(const struct mails_view *v,
			      const shyake_thread *t, int i,
			      const shyake_mail_detail *d)
{
	const shyake_thread_entry *e = &t->entries[i];
	const char *pre = v->g ? v->g->below[i] : NULL;
	int w = v->g ? v->g->width : 0;
	const char *c_lbl = v->no_color ? "" : "\033[1;36m";
	const char *c_rs = v->no_color ? "" : "\033[0m";
	if (!e->mail_id) {
		print_body(pre, w, "This mail was burned or cannot be found.",
			   v->no_color);
		return;
	}

	char date[64];
	cli_format_timestamp(e->timestamp, v->tz_hours, v->time_fmt,
			     v->time_fmt_recent, date, sizeof(date));
	const char *sub = e->subject ? e->subject : "<decryption failed>";
	const char *re = NULL;
	if (e->parent >= 0)
		re = t->entries[e->parent].mail_id ?
			     t->entries[e->parent].mail_id :
			     "(missing)";
	const char *labels[] = { "FROM:", "TO:", "RE:", "DATE:", "SUBJ:" };
	const char *vals[] = { e->sender, e->recipient, re, date, sub };
	for (int k = 0; k < 5; k++) {
		/* the graph already shows what it answers */
		if (k == 2 && (v->g || !re))
			continue;
		if (pre)
			print_graph(pre, w, v->no_color);
		printf("%s%s%s%*s%s\n", c_lbl, labels[k], c_rs,
		       7 - (int)strlen(labels[k]), "", vals[k]);
	}
	if (pre)
		print_graph(pre, -1, v->no_color);
	else
		printf("\n");

	const char *body = d && d->body ? d->body :
			   d		? "<body decryption failed>" :
					  "<body not available>";
	char *copy = strdup(body);
	usize bl = strlen(copy);
	while (bl > 0 && copy[bl - 1] == '\n')
		copy[--bl] = '\0';
	print_body(pre, w, copy, v->no_color);
	free(copy);
}

void cli_render_thread_mails(const shyake_thread *t,
			     shyake_mail_detail *const *details, int flat,
			     int no_color, int plain, int tz_hours,
			     const char *time_fmt, const char *time_fmt_recent)
{
	if (!t || t->count == 0)
		return;
	int n = t->count;
	thread_graph g;
	if (!flat)
		build_graph(t, &g);
	cli_setup_pager(plain);
	struct mails_view v = { flat ? NULL : &g, no_color, tz_hours, time_fmt,
				time_fmt_recent };
	const char *c_lbl = no_color ? "" : "\033[1;36m";
	const char *c_rs = no_color ? "" : "\033[0m";

	for (int i = 0; i < n; i++) {
		const char *id = t->entries[i].mail_id ? t->entries[i].mail_id :
							 "(missing)";
		/* first line: graph node and mail ID */
		if (flat && i > 0)
			printf("\n%s\n\n",
			       "----------------------------------------");
		if (!flat && g.fork[i])
			print_graph(g.fork[i], -1, no_color);
		if (!flat)
			print_graph(g.node[i], g.width, no_color);
		printf("%sID:%s    %s\n", c_lbl, c_rs, id);
		print_thread_mail(&v, t, i, details[i]);
		if (!flat && i + 1 < n)
			print_graph(g.below[i], -1, no_color);
	}
	cli_wait_pager();
	if (!flat)
		free_graph(&g, n);
}
