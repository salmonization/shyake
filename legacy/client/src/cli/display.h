#ifndef SHYAKE_DISPLAY_H
#define SHYAKE_DISPLAY_H

#include <stdint.h>
#include "shyake.h"
#include "internal.h"

/* Pager */
void cli_setup_pager(int disable_pager);
void cli_wait_pager(void);
/* whether output currently goes to the pager */
int cli_pager_active(void);

/* Terminal info */
int cli_get_terminal_width(void);

/* Text formatting */
void cli_print_word_wrap(const char *text, int indent, int width);

/* Terminal columns a UTF-8 string takes (CJK and emoji count 2) */
int cli_utf8_width(const char *s);

/* Copy src into buf cut to max_w columns, "..." marking a cut;
 * returns the columns written */
int cli_utf8_truncate(const char *src, char *buf, int buf_sz, int max_w);

/* Format a UNIX timestamp to a string */
#define TZ_AUTO (0x7FFFFFFF)
void cli_format_timestamp(i64 ts, int tz_hours, const char *fmt,
			  const char *fmt_recent, char *buf, int buf_len);

/* Sender display helper: keep trailing @ for remote parties */
void cli_format_party(const char *party, char *buf, int buf_len);

/* Size display helper: >=1024 -> K */
void cli_format_size(int size, char *buf, int buf_len);

/* Fingerprint display */
void cli_print_fingerprint_hex(const unsigned char *fp);
void cli_print_randomart(const unsigned char *fp);
void cli_render_fingerprint(const char *label, const shyake_fp_result *r,
			    int is_self);

/* Column bitmask for CHECK_COLUMNS */
#define COL_ID (1 << 0)
#define COL_PARTY (1 << 1) /* sender (inbox) or recipient (sent) */
#define COL_SUBJECT (1 << 2)
#define COL_SIZE (1 << 3)
#define COL_DATE (1 << 4)
#define COL_CREATED (1 << 5) /* draft creation timestamp */
#define COL_MODIFIED (1 << 6) /* draft last-modified timestamp */
#define COL_ALL (COL_ID | COL_PARTY | COL_SUBJECT | COL_SIZE | COL_DATE)

/* Mail list rendering */
typedef struct {
	int json_out;
	int csv_out;
	int count_only;
	int no_header;
	int no_color;
	int plain;
	int term_width;
	/* column order: col_order[0..col_count-1] are COL_* values */
	int col_order[7];
	int col_count;
	/* timezone */
	int tz_hours; /* offset hours, or TZ_AUTO */
	const char *time_fmt;
	const char *time_fmt_recent;
	const char *
		id_label; /* override "Mail ID" column header (NULL = default) */
	int local; /* thread from saved mail only: no "(saved)" marks */
} cli_render_opts;

void cli_render_mail_list(const shyake_mail_list *list,
			  const cli_render_opts *opts);

/* Mail detail rendering; re: the mail answered, NULL if none */
void cli_render_mail_detail(const shyake_mail_detail *d, const char *re,
			    int raw, int no_color, int plain, int tz_hours,
			    const char *time_fmt, const char *time_fmt_recent);

/* Single mail header view (check <id>) */
void cli_render_mail_header(const shyake_mail_detail *d, const char *re,
			    int no_color, int tz_hours, const char *time_fmt,
			    const char *time_fmt_recent);

/* Thread views: a lane graph, oldest first (check thread) */
void cli_render_thread_list(const shyake_thread *t,
			    const cli_render_opts *opts);

/* Thread with bodies (fetch thread, read thread); details[i] is the
 * detail of entry i, NULL when it is missing or cannot be read.
 * flat: no graph, a separator line between mails. */
void cli_render_thread_mails(const shyake_thread *t,
			     shyake_mail_detail *const *details, int flat,
			     int no_color, int plain, int tz_hours,
			     const char *time_fmt, const char *time_fmt_recent);

#endif /* SHYAKE_DISPLAY_H */
