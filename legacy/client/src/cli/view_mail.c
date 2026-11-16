/* Mail views: the mailbox list, one mail, one mail's header */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "cJSON.h"
#include "display.h"

/* ------------------------------------------------------------------ */
/* Mail list rendering                                                */
/* ------------------------------------------------------------------ */

/* columns by index: the bit position of their COL_* flag */
#define NCOLS 7
enum { C_ID, C_PARTY, C_SUBJECT, C_SIZE, C_DATE, C_CREATED, C_MODIFIED };

/* space after a column when the subject is fitted to the terminal */
static const int col_gap[NCOLS] = { 1, 1, 0, 1, 0, 1, 0 };

/* column index of a COL_* flag, -1 for anything else */
static int col_index(int col)
{
	for (int i = 0; i < NCOLS; i++)
		if (col == 1 << i)
			return i;
	return -1;
}

/* one mail of a list as display strings, by column index */
typedef struct {
	char *cell[NCOLS];
} list_row;

static list_row *format_rows(const shyake_mail_list *list,
			     const cli_render_opts *opts)
{
	list_row *rows = calloc(list->count, sizeof(list_row));
	for (int i = 0; i < list->count; i++) {
		const shyake_mail_entry *e = &list->entries[i];
		char **cell = rows[i].cell;
		char pbuf[256], sbuf[16], dbuf[64], cbuf[64];
		cli_format_party(e->party, pbuf, sizeof(pbuf));
		cli_format_size(e->size, sbuf, sizeof(sbuf));
		cli_format_timestamp(e->timestamp, opts->tz_hours,
				     opts->time_fmt, opts->time_fmt_recent,
				     dbuf, sizeof(dbuf));
		cli_format_timestamp(e->created, opts->tz_hours, opts->time_fmt,
				     opts->time_fmt_recent, cbuf, sizeof(cbuf));
		cell[C_ID] = strdup(e->mail_id);
		cell[C_PARTY] = strdup(pbuf);
		cell[C_SUBJECT] =
			strdup(e->subject ? e->subject : "<decryption failed>");
		cell[C_SIZE] = strdup(sbuf);
		cell[C_DATE] = strdup(dbuf);
		cell[C_CREATED] = strdup(cbuf);
		cell[C_MODIFIED] = strdup(dbuf);
	}
	return rows;
}

static void free_rows(list_row *rows, int count)
{
	for (int i = 0; i < count; i++)
		for (int c = 0; c < NCOLS; c++)
			free(rows[i].cell[c]);
	free(rows);
}

static void print_list_json(const list_row *rows, int count, int is_sent)
{
	cJSON *arr = cJSON_CreateArray();
	for (int i = 0; i < count; i++) {
		char *const *cell = rows[i].cell;
		cJSON *obj = cJSON_CreateObject();
		cJSON_AddStringToObject(obj, "mail_id", cell[C_ID]);
		cJSON_AddStringToObject(obj, is_sent ? "recipient" : "sender",
					cell[C_PARTY]);
		cJSON_AddStringToObject(obj, "subject", cell[C_SUBJECT]);
		cJSON_AddStringToObject(obj, "size", cell[C_SIZE]);
		cJSON_AddStringToObject(obj, "date", cell[C_DATE]);
		cJSON_AddItemToArray(arr, obj);
	}
	char *out = cJSON_PrintUnformatted(arr);
	printf("%s\n", out);
	free(out);
	cJSON_Delete(arr);
}

static void print_list_csv(const list_row *rows, int count,
			   const cli_render_opts *opts, const char *id_hdr,
			   int is_sent)
{
	if (!opts->no_header)
		printf("%s,%s,Subject,Size,Date\n", id_hdr,
		       is_sent ? "To" : "From");
	for (int i = 0; i < count; i++) {
		char *const *cell = rows[i].cell;
		const char *sub = cell[C_SUBJECT];
		char escaped[2048] = { 0 };
		int p = 0;
		for (int j = 0; sub[j] && p < 2040; j++) {
			if (sub[j] == '"')
				escaped[p++] = '"';
			escaped[p++] = sub[j];
		}
		printf("%s,%s,\"%s\",%s,%s\n", cell[C_ID], cell[C_PARTY],
		       escaped, cell[C_SIZE], cell[C_DATE]);
	}
}

/* widest cell of each active column; the subject fits the terminal */
static void column_widths(const list_row *rows, int count, unsigned active,
			  const char *const *label, const cli_render_opts *opts,
			  int *w)
{
	for (int c = 0; c < NCOLS; c++)
		w[c] = (int)strlen(label[c]);
	for (int i = 0; i < count; i++) {
		for (int c = 0; c < NCOLS; c++) {
			if (!(active & (1u << c)))
				continue;
			const char *cell = rows[i].cell[c];
			int l = c == C_SUBJECT ? cli_utf8_width(cell) :
						 (int)strlen(cell);
			if (l > w[c])
				w[c] = l;
		}
	}
	if (opts->plain || !(active & COL_SUBJECT))
		return;

	int tw = opts->term_width > 0 ? opts->term_width :
					cli_get_terminal_width();
	int used = 0;
	for (int c = 0; c < NCOLS; c++)
		if (c != C_SUBJECT && (active & (1u << c)))
			used += w[c] + col_gap[c];
	int max_sub = tw - used - 1;
	if (max_sub < 15)
		max_sub = 15;
	if (w[C_SUBJECT] > max_sub)
		w[C_SUBJECT] = max_sub;
}

static void print_list_header(const int *order, int ncols,
			      const char *const *label, const int *w,
			      int no_color)
{
	if (!no_color)
		printf("\033[0m");
	for (int k = 0; k < ncols; k++) {
		int c = col_index(order[k]);
		if (c < 0)
			continue;
		const char *sep = k == ncols - 1 ? "" : " ";
		if (no_color)
			printf("%-*s%s", w[c], label[c], sep);
		else
			printf("\033[39m\033[4m%s\033[24m%-*s%s", label[c],
			       w[c] - (int)strlen(label[c]), "", sep);
	}
	printf("\n");
}

static void print_list_row(const shyake_mail_entry *e, const list_row *r,
			   const int *order, int ncols, const int *w,
			   int no_color)
{
	const char *c_w = no_color ? "" : "\033[39m";
	const char *c_cy = no_color ? "" : "\033[36m";
	const char *c_rs = no_color ? "" : "\033[0m";
	const char *c_sz = no_color	   ? "" :
			   e->size >= 1024 ? "\033[1;35m" :
					     "\033[35m";

	for (int k = 0; k < ncols; k++) {
		int c = col_index(order[k]);
		if (c < 0)
			continue;
		const char *sep = k == ncols - 1 ? "" : " ";
		const char *text = r->cell[c];
		if (c == C_PARTY) {
			/* a remote party keeps its @ uncolored */
			int len = (int)strlen(text);
			if (len > 0 && text[len - 1] == '@')
				printf("%s%.*s%s@", c_cy, len - 1, text, c_w);
			else
				printf("%s%s", c_cy, text);
			printf("%s%-*s%s", c_rs, w[c] - len, "", sep);
		} else if (c == C_SUBJECT) {
			/* pad to the width with spaces (byte printf won't) */
			char sub[512];
			int dw =
				cli_utf8_truncate(text, sub, sizeof(sub), w[c]);
			printf("%s%s%*s%s%s", c_w, sub, w[c] - dw, "", c_rs,
			       sep);
		} else {
			const char *color = c == C_ID	? c_rs :
					    c == C_SIZE ? c_sz :
							  c_w;
			printf("%s%-*s%s%s", color, w[c], text, c_rs, sep);
		}
	}
	printf("\n");
}

static void print_list_table(const shyake_mail_list *list, const list_row *rows,
			     const cli_render_opts *opts, const char *id_hdr,
			     int is_sent)
{
	cli_setup_pager(opts->plain);

	/* active columns in order; all defaults if col_count == 0 */
	static const int def_order[] = { COL_ID, COL_PARTY, COL_SUBJECT,
					 COL_SIZE, COL_DATE };
	const int *order = opts->col_count > 0 ? opts->col_order : def_order;
	int ncols = opts->col_count > 0 ? opts->col_count : 5;
	unsigned active = 0;
	for (int k = 0; k < ncols; k++)
		active |= (unsigned)order[k];

	const char *label[NCOLS] = { id_hdr,	is_sent ? "To" : "From",
				     "Subject", "Size",
				     "Date",	"Created",
				     "Modified" };
	int w[NCOLS];
	column_widths(rows, list->count, active, label, opts, w);

	if (!opts->no_header)
		print_list_header(order, ncols, label, w, opts->no_color);
	for (int i = 0; i < list->count; i++)
		print_list_row(&list->entries[i], &rows[i], order, ncols, w,
			       opts->no_color);
	if (!opts->no_header)
		printf("\nTotal: %d item%s\n", list->count,
		       list->count == 1 ? "" : "s");
	cli_wait_pager();
}

void cli_render_mail_list(const shyake_mail_list *list,
			  const cli_render_opts *opts)
{
	if (!list)
		return;
	int count = list->count;
	if (opts->count_only) {
		printf("%d\n", count);
		return;
	}
	if (count == 0) {
		if (opts->json_out)
			printf("[]\n");
		else if (!opts->csv_out)
			printf("No mail found.\n");
		return;
	}

	int is_sent = list->entries[0].is_sent;
	const char *id_hdr = opts->id_label && opts->id_label[0] ?
				     opts->id_label :
				     "Mail ID";
	list_row *rows = format_rows(list, opts);
	if (opts->json_out)
		print_list_json(rows, count, is_sent);
	else if (opts->csv_out)
		print_list_csv(rows, count, opts, id_hdr, is_sent);
	else
		print_list_table(list, rows, opts, id_hdr, is_sent);
	free_rows(rows, count);
}

/* ------------------------------------------------------------------ */
/* Mail detail (fetch --raw or normal)                                */
/* ------------------------------------------------------------------ */

void cli_render_mail_detail(const shyake_mail_detail *d, const char *re,
			    int raw, int no_color, int plain, int tz_hours,
			    const char *time_fmt, const char *time_fmt_recent)
{
	if (!d)
		return;

	if (raw) {
		if (d->body)
			printf("%s", d->body);
		return;
	}

	cli_setup_pager(plain);
	const char *c_lbl = no_color ? "" : "\033[1;36m";
	const char *c_val = no_color ? "" : "\033[0m";
	int tw = cli_get_terminal_width();

	char date_buf[64];
	cli_format_timestamp(d->timestamp, tz_hours, time_fmt, time_fmt_recent,
			     date_buf, sizeof(date_buf));

	const char *sub_text = d->subject ? d->subject : "<decryption failed>";

	printf("%sFROM:%s  %s\n", c_lbl, c_val, d->sender);
	printf("%sTO:%s    %s\n", c_lbl, c_val, d->recipient);
	if (re)
		printf("%sRE:%s    %s\n", c_lbl, c_val, re);
	printf("%sDATE:%s  %s\n", c_lbl, c_val, date_buf);
	printf("%sSUBJ:%s  ", c_lbl, c_val);
	cli_print_word_wrap(sub_text, 7, tw);
	printf("\n");

	if (d->body) {
		printf("%s", d->body);
		if (isatty(STDOUT_FILENO) || cli_pager_active()) {
			printf("\n");
		}
	} else {
		printf("<body decryption failed>\n");
	}

	cli_wait_pager();
}

/* ------------------------------------------------------------------ */
/* Mail header view (check <id>)                                      */
/* ------------------------------------------------------------------ */

void cli_render_mail_header(const shyake_mail_detail *d, const char *re,
			    int no_color, int tz_hours, const char *time_fmt,
			    const char *time_fmt_recent)
{
	if (!d)
		return;

	const char *c_lbl = no_color ? "" : "\033[1;36m";
	const char *c_rs = no_color ? "" : "\033[0m";
	int tw = cli_get_terminal_width();

	char mod_buf[64], crt_buf[64];
	cli_format_timestamp(d->timestamp, tz_hours, time_fmt, time_fmt_recent,
			     mod_buf, sizeof(mod_buf));

	const char *sub_text = d->subject ? d->subject : "(decryption failed)";

	printf("%sFROM:%s %s\n", c_lbl, c_rs, d->sender);
	printf("%sTO:%s   %s\n", c_lbl, c_rs, d->recipient);
	if (re)
		printf("%sRE:%s   %s\n", c_lbl, c_rs, re);
	printf("%sSUBJ:%s ", c_lbl, c_rs);
	cli_print_word_wrap(sub_text, 6, tw);
	printf("%sSIZE:%s %d\n", c_lbl, c_rs, d->size);
	if (d->created) {
		cli_format_timestamp(d->created, tz_hours, time_fmt,
				     time_fmt_recent, crt_buf, sizeof(crt_buf));
		printf("%sCRT:%s  %s\n", c_lbl, c_rs, crt_buf);
		printf("%sMOD:%s  %s\n", c_lbl, c_rs, mod_buf);
	} else {
		printf("%sDATE:%s %s\n", c_lbl, c_rs, mod_buf);
	}
}
