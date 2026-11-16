#ifndef SHYAKE_CLI_COMPOSE_H
#define SHYAKE_CLI_COMPOSE_H

#include <stdio.h>
#include "shyake.h"
#include "internal.h"
#include "config.h"

/* Writing mail in an editor. Plaintext only ever sits in private
 * temp files in the config dir, zeroed before they are removed. */

/* Whole stream, allocated; *out_len receives its length. */
u8 *cli_read_all(FILE *f, usize *out_len);

/*
 * Let the user edit a draft template. orig, the mail answered, is
 * shown beside it in vim/nvim and above a scissors line elsewhere.
 * Returns the edited text without the scissors part, or NULL when
 * the edit is aborted (the reason is printed).
 */
char *cli_edit_draft(const app_config *cfg, const char *config_dir,
		     const char *initial, const char *orig);

/* Split a To:/Subject:/---/body template in place; -1 without "---". */
int cli_parse_compose(char *buf, char **to, char **subject, char **body);

/* orig above a scissors line, then the template initial; a
 * scissors line inside orig is indented so it cannot cut there. */
char *cli_with_scissors(const char *initial, const char *orig);

/* Text after the scissors line, or NULL when it is gone. */
const char *cli_after_scissors(const char *buf);

/* The mail answered, as fetch shows it, for the editor. */
char *cli_format_original(const shyake_mail_detail *d, const app_config *cfg);

#endif /* SHYAKE_CLI_COMPOSE_H */
