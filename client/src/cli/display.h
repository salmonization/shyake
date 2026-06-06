#ifndef SHYAKE_DISPLAY_H
#define SHYAKE_DISPLAY_H

#include <stdint.h>
#include "shyake.h"

/* Pager */
void cli_setup_pager(int disable_pager);
void cli_wait_pager(void);

/* Terminal info */
int cli_get_terminal_width(void);

/* Text formatting */
void cli_print_word_wrap(const char *text, int indent, int width);

/*
 * Format a UNIX timestamp to a string.
 * tz_hours: offset in whole hours (e.g. 8 = UTC+8, -6 = UTC-6).
 *           INT_MIN means "auto" (use system localtime).
 * fmt / fmt_recent: strftime patterns.
 *   fmt_recent is used when the mail is < 180 days old; may be NULL.
 * buf / buf_len: output buffer.
 */
#define TZ_AUTO (0x7FFFFFFF)
void cli_format_timestamp(int64_t ts, int tz_hours,
                          const char *fmt, const char *fmt_recent,
                          char *buf, int buf_len);

/* Sender display helper: keep trailing @ for remote parties */
void cli_format_party(const char *party, char *buf, int buf_len);

/* Size display helper: >=1024 -> K */
void cli_format_size(int size, char *buf, int buf_len);

/* Fingerprint display */
void cli_print_fingerprint_hex(const unsigned char *fp);
void cli_print_randomart(const unsigned char *fp);
void cli_render_fingerprint(const char *label,
                            const shyake_fp_result *r,
                            int is_self);

/* Column bitmask for CHECK_COLUMNS */
#define COL_ID      (1 << 0)
#define COL_PARTY   (1 << 1)  /* sender (inbox) or recipient (sent) */
#define COL_SUBJECT (1 << 2)
#define COL_SIZE    (1 << 3)
#define COL_DATE    (1 << 4)
#define COL_ALL     (COL_ID | COL_PARTY | COL_SUBJECT | COL_SIZE | COL_DATE)

/* Mail list rendering */
typedef struct {
    int json_out;
    int csv_out;
    int count_only;
    int no_header;
    int no_color;
    int plain;
    int term_width;
    /* column order: col_order[0..col_count-1] are COL_* values
     * in the order they should appear. col_count=0 means show all
     * in default order. */
    int col_order[5];
    int col_count;
    /* timezone */
    int tz_hours;           /* offset hours, or TZ_AUTO */
    const char *time_fmt;
    const char *time_fmt_recent;
} cli_render_opts;

void cli_render_mail_list(const shyake_mail_list *list,
                          const cli_render_opts *opts);

/* Mail detail rendering */
void cli_render_mail_detail(const shyake_mail_detail *d,
                            int raw, int no_color, int plain,
                            int tz_hours,
                            const char *time_fmt,
                            const char *time_fmt_recent);

/* Single mail header view (check <id>) */
void cli_render_mail_header(const shyake_mail_detail *d,
                            int no_color, int tz_hours,
                            const char *time_fmt,
                            const char *time_fmt_recent);

#endif /* SHYAKE_DISPLAY_H */
