#ifndef SHYAKE_CLI_H
#define SHYAKE_CLI_H

/*
 * What the commands share. Each cmd_* runs one command: argv as
 * typed (argv[1] the command), the config with the global flags
 * folded in, and the config directory. It returns an exit status.
 */

#include "shyake.h"
#include "internal.h"
#include "config.h"
#include "display.h"

#ifndef SHYAKE_VERSION
#define SHYAKE_VERSION "dev"
#endif

/* longest subject this client sends, in bytes */
#define SUBJECT_MAX 128

/* common.c */
/* print "<action> <reason>"; the reason may be empty */
void print_failure(const char *action, const char *reason);
/* action failed in the library, e.g. "Send failed." */
void print_lib_error(shyake_ctx *ctx, const char *action);
/* action failed in the local drafts store */
void print_drafts_error(const char *action);
/* a send failed; a changed key is fatal and says how to check it */
void print_send_error(shyake_ctx *ctx, shyake_err ret, const char *recipient);
/* the RE: line of a reply: where the mail it answers is now */
char *describe_reply(shyake_ctx *ctx, const shyake_mail_detail *d);
/* why a subject cannot be sent, or NULL when it can */
const char *subject_problem(const char *subject);
/* a retry cannot help when you are blocked or the recipient is gone */
int retry_may_help(shyake_err ret);
/* library settings; a value missing from the config is "" */
shyake_config lib_config(const app_config *cfg, const char *config_dir);
/* a context with the passphrase entered; NULL when it was not */
shyake_ctx *open_ctx(const shyake_config *c);
/* the config names an account on an instance; says so when not */
int need_account(const app_config *cfg);
int need_username(const app_config *cfg);
/* the account's context, for a command that talks to the instance */
shyake_ctx *account_ctx(const app_config *cfg, const char *config_dir);
/* list rendering options from the config */
cli_render_opts render_opts(const app_config *cfg);
/* check <id>: one mail without its body */
void show_header(shyake_ctx *ctx, const shyake_mail_detail *d,
		 const app_config *cfg);
/* fetch, read: one mail in full; raw prints the body alone */
void show_mail(shyake_ctx *ctx, const shyake_mail_detail *d, int raw,
	       const app_config *cfg);
/* --raw, the only option of fetch and read; leaves optind */
int parse_raw(int argc, char *argv[]);
/* a body from file, or stdin without one; NULL after saying why */
u8 *read_body(const char *file, const char *action, usize *len);

/* cmd_account.c, cmd_send.c, cmd_read.c, cmd_tools.c */
int cmd_whoami(int argc, char *argv[], const app_config *cfg,
	       const char *config_dir);
int cmd_register(int argc, char *argv[], const app_config *cfg,
		 const char *config_dir);
int cmd_rotate(int argc, char *argv[], const app_config *cfg,
	       const char *config_dir);
int cmd_destroy(int argc, char *argv[], const app_config *cfg,
		const char *config_dir);
int cmd_fingerprint(int argc, char *argv[], const app_config *cfg,
		    const char *config_dir);
int cmd_block(int argc, char *argv[], const app_config *cfg,
	      const char *config_dir);
int cmd_blocklist(int argc, char *argv[], const app_config *cfg,
		  const char *config_dir);
int cmd_send(int argc, char *argv[], const app_config *cfg,
	     const char *config_dir);
int cmd_reply(int argc, char *argv[], const app_config *cfg,
	      const char *config_dir);
int cmd_compose(int argc, char *argv[], const app_config *cfg,
		const char *config_dir);
int cmd_check(int argc, char *argv[], const app_config *cfg,
	      const char *config_dir);
int cmd_fetch(int argc, char *argv[], const app_config *cfg,
	      const char *config_dir);
int cmd_read(int argc, char *argv[], const app_config *cfg,
	     const char *config_dir);
int cmd_save(int argc, char *argv[], const app_config *cfg,
	     const char *config_dir);
int cmd_burn(int argc, char *argv[], const app_config *cfg,
	     const char *config_dir);
/* check, fetch, save or read thread <id> */
int cmd_thread(int argc, char *argv[], const app_config *cfg,
	       const char *config_dir);
int cmd_enc(int argc, char *argv[], const app_config *cfg,
	    const char *config_dir);
int cmd_dec(int argc, char *argv[], const app_config *cfg,
	    const char *config_dir);
int cmd_update(int argc, char *argv[], const app_config *cfg,
	       const char *config_dir);

#endif /* SHYAKE_CLI_H */
