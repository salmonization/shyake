#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "internal.h"
#include "display.h"
#include "config.h"

char *cli_trim(char *str)
{
	while (*str == ' ' || *str == '\t' || *str == '\r' || *str == '\n')
		str++;
	if (*str == '\0')
		return str;
	char *end = str + strlen(str) - 1;
	while (end > str &&
	       (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n'))
		end--;
	end[1] = '\0';
	return str;
}

void cli_parse_columns(const char *spec, int *col_order, int *col_count)
{
	static const int default_order[] = { COL_ID, COL_PARTY, COL_SUBJECT,
					     COL_SIZE, COL_DATE };
	if (!spec || spec[0] == '\0') {
		for (int i = 0; i < 5; i++)
			col_order[i] = default_order[i];
		*col_count = 5;
		return;
	}

	*col_count = 0;
	char buf[256];
	snprintf(buf, sizeof(buf), "%s", spec);
	char *rest = buf;
	while (rest && *col_count < 5) {
		char *tok = rest;
		char *comma = strchr(rest, ',');
		if (comma) {
			*comma = '\0';
			rest = comma + 1;
		} else {
			rest = NULL;
		}
		while (*tok == ' ')
			tok++;
		int col = 0;
		if (strcmp(tok, "id") == 0)
			col = COL_ID;
		else if (strcmp(tok, "sender") == 0 ||
			 strcmp(tok, "from") == 0 || strcmp(tok, "to") == 0 ||
			 strcmp(tok, "recipient") == 0)
			col = COL_PARTY;
		else if (strcmp(tok, "subject") == 0)
			col = COL_SUBJECT;
		else if (strcmp(tok, "size") == 0)
			col = COL_SIZE;
		else if (strcmp(tok, "date") == 0)
			col = COL_DATE;

		if (col)
			col_order[(*col_count)++] = col;
	}

	if (*col_count == 0) {
		for (int i = 0; i < 5; i++)
			col_order[i] = default_order[i];
		*col_count = 5;
	}
}

void cli_free_config(app_config *config)
{
	if (!config)
		return;
	free(config->instance);
	free(config->username);
	free(config->time_format);
	free(config->time_format_recent);
	free(config->check_columns);
	free(config->editor);
	free(config);
}

/*
 * Split a "KEY=value" line in place. Blank lines and # comments
 * have no key. A value in matching quotes loses them.
 */
static int split_line(char *line, char **key, char **val)
{
	char *trimmed = cli_trim(line);
	if (trimmed[0] == '#' || trimmed[0] == '\0')
		return 0;
	char *equals = strchr(trimmed, '=');
	if (!equals)
		return 0;
	*equals = '\0';
	*key = cli_trim(trimmed);
	*val = cli_trim(equals + 1);

	char *v = *val;
	usize len = strlen(v);
	if ((v[0] == '"' || v[0] == '\'') && v[0] == v[len - 1] && len >= 2) {
		v[len - 1] = '\0';
		*val = v + 1;
	}
	return 1;
}

/* an empty value leaves the setting unset */
static void set_str(char **field, const char *val)
{
	free(*field);
	*field = val[0] ? strdup(val) : NULL;
}

/* one setting; unknown keys are ignored */
static void apply_setting(app_config *cfg, const char *key, const char *val)
{
	if (strcmp(key, "INSTANCE") == 0)
		set_str(&cfg->instance, val);
	else if (strcmp(key, "USERNAME") == 0)
		set_str(&cfg->username, val);
	else if (strcmp(key, "TIME_FORMAT") == 0)
		set_str(&cfg->time_format, val);
	else if (strcmp(key, "TIME_FORMAT_RECENT") == 0)
		set_str(&cfg->time_format_recent, val);
	else if (strcmp(key, "CHECK_COLUMNS") == 0)
		set_str(&cfg->check_columns, val);
	else if (strcmp(key, "EDITOR") == 0)
		set_str(&cfg->editor, val);
	else if (strcmp(key, "NO_COLOR") == 0)
		cfg->no_color = atoi(val);
	else if (strcmp(key, "DEFAULT_ACTION") == 0)
		cfg->default_action = atoi(val);
	else if (strcmp(key, "TIME_ZONE") == 0)
		/* auto: system localtime; an integer: UTC offset hours */
		cfg->tz_hours = strcmp(val, "auto") == 0 || val[0] == '\0' ?
					TZ_AUTO :
					atoi(val);
}

app_config *cli_read_config(const char *config_dir)
{
	app_config *cfg = calloc(1, sizeof(app_config));
	if (!cfg)
		return NULL;
	cfg->tz_hours = TZ_AUTO;

	char path[512];
	snprintf(path, sizeof(path), "%s/config", config_dir);
	FILE *f = fopen(path, "r");
	if (!f)
		return cfg;

	char line[1024];
	char *key, *val;
	while (fgets(line, sizeof(line), f))
		if (split_line(line, &key, &val))
			apply_setting(cfg, key, val);
	fclose(f);
	return cfg;
}

/* the config file's lines, each with its line end */
static char **read_lines(const char *path, int *count)
{
	char **lines = NULL;
	*count = 0;
	FILE *f = fopen(path, "r");
	if (!f)
		return NULL;
	char buf[1024];
	while (fgets(buf, sizeof(buf), f)) {
		lines = realloc(lines, sizeof(char *) * (*count + 1));
		lines[(*count)++] = strdup(buf);
	}
	fclose(f);
	return lines;
}

int cli_set_account(const char *config_dir, const char *username,
		    const char *instance)
{
	char path[512];
	snprintf(path, sizeof(path), "%s/config", config_dir);
	int count;
	char **lines = read_lines(path, &count);

	/* rewrite INSTANCE in place; USERNAME follows it */
	int has_instance = 0;
	for (int i = 0; i < count; i++) {
		char copy[1024], *key, *val;
		snprintf(copy, sizeof(copy), "%s", lines[i]);
		if (!split_line(copy, &key, &val))
			continue;
		if (strcmp(key, "INSTANCE") == 0) {
			char new_line[1024];
			snprintf(new_line, sizeof(new_line),
				 "INSTANCE=%s\nUSERNAME=%s\n", instance,
				 username);
			free(lines[i]);
			lines[i] = strdup(new_line);
			has_instance = 1;
		} else if (strcmp(key, "USERNAME") == 0) {
			free(lines[i]);
			lines[i] = strdup("");
		}
	}

	FILE *f = fopen(path, "w");
	for (int i = 0; i < count; i++) {
		if (f)
			fputs(lines[i], f);
		free(lines[i]);
	}
	free(lines);
	if (!f)
		return -1;
	if (!has_instance)
		fprintf(f, "INSTANCE=%s\nUSERNAME=%s\n", instance, username);
	fclose(f);
	return 0;
}
