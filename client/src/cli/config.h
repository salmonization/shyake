#ifndef SHYAKE_CLI_CONFIG_H
#define SHYAKE_CLI_CONFIG_H

/* Settings from <config_dir>/config, one KEY=value per line */
typedef struct {
	char *instance;
	char *username;
	char *time_format;
	char *time_format_recent;
	char *check_columns;
	char *editor;
	int no_color; /* NO_COLOR=1, --no-color, --plain or $NO_COLOR */
	int plain; /* --plain: no pager, color or truncation */
	int debug; /* --debug: libcurl logs to stderr */
	int tz_hours;
	int default_action;
} app_config;

/* Read the config file; a missing file gives the defaults and an
 * empty value leaves its setting unset. */
app_config *cli_read_config(const char *config_dir);
void cli_free_config(app_config *config);

/* Point the config at a new account: INSTANCE and USERNAME. */
int cli_set_account(const char *config_dir, const char *username,
		    const char *instance);

/* CHECK_COLUMNS ("id,from,subject,...") as COL_* flags, in order;
 * the default columns when spec names none. */
void cli_parse_columns(const char *spec, int *col_order, int *col_count);

/* Strip surrounding blanks and line ends in place. */
char *cli_trim(char *str);

#endif /* SHYAKE_CLI_CONFIG_H */
