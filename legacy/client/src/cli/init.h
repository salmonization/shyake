#ifndef SHYAKE_CLI_INIT_H
#define SHYAKE_CLI_INIT_H

/* Default config directory, allocated; NULL when there is none. */
char *get_config_dir(void);

/* shyake init: config file and key pairs. Returns 0 on success. */
int cmd_init(const char *config_dir);

#endif /* SHYAKE_CLI_INIT_H */
