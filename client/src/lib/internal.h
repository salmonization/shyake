#ifndef SHYAKE_INTERNAL_H
#define SHYAKE_INTERNAL_H

#include "shyake.h"

/* Internal context definition hidden from the public ABI */
struct shyake_ctx {
	char *instance_url;
	char *config_dir;
	char *username;
	char *time_format;
	char *time_format_recent;
	char *check_columns;
	int plain;
	int debug;
	int no_color;
};

#endif /* SHYAKE_INTERNAL_H */
