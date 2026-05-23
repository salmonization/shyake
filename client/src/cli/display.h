#ifndef SHYAKE_DISPLAY_H
#define SHYAKE_DISPLAY_H

#include "shyake.h"

/* Pager */
void cli_setup_pager(int disable_pager);
void cli_wait_pager(void);

/* Terminal info */
int cli_get_terminal_width(void);

/* Text formatting */
void cli_print_word_wrap(const char *text, int indent, int width);

/* Fingerprint display */
void cli_print_fingerprint_hex(const unsigned char *fp);
void cli_print_randomart(const unsigned char *fp);
void cli_render_fingerprint(const char *label,
                            const shyake_fp_result *r,
                            int is_self);

/* Mail list rendering */
typedef struct {
    int json_out;
    int csv_out;
    int count_only;
    int no_header;
    int no_color;
    int plain;
    int term_width;
} cli_render_opts;

void cli_render_mail_list(const shyake_mail_list *list,
                          const cli_render_opts *opts);

/* Mail detail rendering */
void cli_render_mail_detail(const shyake_mail_detail *d,
                            int raw, int no_color, int plain);

/* Single mail header view (check <id>) */
void cli_render_mail_header(const shyake_mail_detail *d,
                            int no_color);

#endif /* SHYAKE_DISPLAY_H */
