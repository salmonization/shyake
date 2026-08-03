#ifndef SHYAKE_CLI_MAN_H
#define SHYAKE_CLI_MAN_H

/* Print the "shyake man" overview (subcmd == NULL) or a single
 * command's detailed usage (subcmd != NULL, unknown names print
 * "Unknown command"). */
void cli_print_man(const char *subcmd);

#endif /* SHYAKE_CLI_MAN_H */
