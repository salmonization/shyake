#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include "shyake.h"
#include "display.h"

int cmd_init(const char *config_dir);

char *get_config_dir(void);

static uint8_t* read_all_bytes(FILE *f, size_t *out_len)
{
    size_t capacity = 1024;
    size_t length = 0;
    uint8_t *buffer = malloc(capacity);
    if (!buffer) return NULL;

    while (!feof(f) && !ferror(f)) {
        if (length == capacity) {
            capacity *= 2;
            uint8_t *new_buffer = realloc(buffer, capacity);
            if (!new_buffer) {
                free(buffer);
                return NULL;
            }
            buffer = new_buffer;
        }
        size_t read_bytes = fread(buffer + length, 1, capacity - length, f);
        length += read_bytes;
    }
    *out_len = length;
    return buffer;
}

int global_plain = 0;
int global_debug = 0;
int global_no_color = 0;
char *global_config_dir = NULL;

typedef struct {
    char *instance;
    char *username;
    char *time_format;
    char *time_format_recent;
    char *check_columns;
    int no_color;
    int tz_hours;
    int default_action;
} app_config;

static char*
trim_whitespace(char *str)
{
    // trim leading and trailing spaces
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

static void
parse_check_columns(const char *spec, int *col_order, int *col_count)
{
    static const int default_order[] = {
        COL_ID, COL_PARTY, COL_SUBJECT, COL_SIZE, COL_DATE
    };
    if (!spec || spec[0] == '\0') {
        for (int i = 0; i < 5; i++) col_order[i] = default_order[i];
        *col_count = 5;
        return;
    }

    *col_count = 0;
    char buf[256];
    snprintf(buf, sizeof(buf), "%s", spec);
    char *tok = strtok(buf, ",");
    while (tok && *col_count < 5) {
        while (*tok == ' ') tok++;
        int col = 0;
        if (strcmp(tok, "id") == 0)
            col = COL_ID;
        else if (strcmp(tok, "sender")    == 0 ||
                 strcmp(tok, "from")      == 0 ||
                 strcmp(tok, "to")        == 0 ||
                 strcmp(tok, "recipient") == 0)
            col = COL_PARTY;
        else if (strcmp(tok, "subject") == 0)
            col = COL_SUBJECT;
        else if (strcmp(tok, "size") == 0)
            col = COL_SIZE;
        else if (strcmp(tok, "date") == 0)
            col = COL_DATE;

        if (col) col_order[(*col_count)++] = col;
        tok = strtok(NULL, ",");
    }

    if (*col_count == 0) {
        for (int i = 0; i < 5; i++) col_order[i] = default_order[i];
        *col_count = 5;
    }
}

static void
free_app_config(app_config *config)
{
    // free configuration fields
    if (!config)
        return;
    free(config->instance);
    free(config->username);
    free(config->time_format);
    free(config->time_format_recent);
    free(config->check_columns);
    free(config);
}

static app_config*
read_config(const char *config_dir)
{
    // read and parse config file
    app_config *cfg = calloc(1, sizeof(app_config));
    if (!cfg)
        return NULL;
    cfg->tz_hours = TZ_AUTO; // default: system localtime

    char path[512];
    snprintf(path, sizeof(path), "%s/config", config_dir);
    FILE *f = fopen(path, "r");
    if (!f)
        return cfg;

    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        char *trimmed = trim_whitespace(line);
        if (trimmed[0] == '#' || trimmed[0] == '\0')
            continue;

        char *equals = strchr(trimmed, '=');
        if (!equals)
            continue;

        *equals = '\0';
        char *key = trim_whitespace(trimmed);
        char *val = trim_whitespace(equals + 1);

        if ((val[0] == '"' || val[0] == '\'') &&
            val[0] == val[strlen(val) - 1] &&
            strlen(val) >= 2) {
            val[strlen(val) - 1] = '\0';
            val++;
        }

        if (strcmp(key, "INSTANCE") == 0) {
            free(cfg->instance);
            cfg->instance = strdup(val);
        } else if (strcmp(key, "USERNAME") == 0) {
            free(cfg->username);
            cfg->username = strdup(val);
        } else if (strcmp(key, "TIME_FORMAT") == 0) {
            free(cfg->time_format);
            cfg->time_format = strdup(val);
        } else if (strcmp(key, "TIME_FORMAT_RECENT") == 0) {
            free(cfg->time_format_recent);
            cfg->time_format_recent = strdup(val);
        } else if (strcmp(key, "CHECK_COLUMNS") == 0) {
            free(cfg->check_columns);
            cfg->check_columns = strdup(val);
        } else if (strcmp(key, "NO_COLOR") == 0) {
            cfg->no_color = atoi(val);
        } else if (strcmp(key, "TIME_ZONE") == 0) {
            // auto    = system localtime
            // integer = UTC offset hours
            if (strcmp(val, "auto") == 0 || val[0] == '\0')
                cfg->tz_hours = TZ_AUTO;
            else
                cfg->tz_hours = atoi(val);
        } else if (strcmp(key, "DEFAULT_ACTION") == 0) {
            cfg->default_action = atoi(val);
        }
    }
    fclose(f);
    return cfg;
}



static int
update_config_user_and_instance(const char *config_dir,
                                const char *username,
                                const char *instance)
{
    // update INSTANCE key in config file in-place
    char path[512];
    snprintf(path, sizeof(path), "%s/config", config_dir);

    FILE *f = fopen(path, "r");
    char **lines = NULL;
    int count = 0;
    if (f) {
        char line_buf[1024];
        while (fgets(line_buf, sizeof(line_buf), f)) {
            lines = realloc(lines, sizeof(char*) * (count + 1));
            lines[count++] = strdup(line_buf);
        }
        fclose(f);
    }

    int has_instance = 0;
    for (int i = 0; i < count; i++) {
        char line_copy[1024];
        strcpy(line_copy, lines[i]);
        char *trimmed = trim_whitespace(line_copy);
        if (trimmed[0] == '#' || trimmed[0] == '\0')
            continue;
        char *equals = strchr(trimmed, '=');
        if (!equals)
            continue;
        *equals = '\0';
        char *key = trim_whitespace(trimmed);
        if (strcmp(key, "INSTANCE") == 0) {
            free(lines[i]);
            char new_line[1024];
            snprintf(new_line, sizeof(new_line),
                     "INSTANCE=%s\nUSERNAME=%s\n",
                     instance, username);
            lines[i] = strdup(new_line);
            has_instance = 1;
        } else if (strcmp(key, "USERNAME") == 0) {
            free(lines[i]);
            lines[i] = strdup(""); // clear old USERNAME line
        }
    }

    f = fopen(path, "w");
    if (!f) {
        for (int i = 0; i < count; i++)
            free(lines[i]);
        free(lines);
        return -1;
    }

    for (int i = 0; i < count; i++) {
        fputs(lines[i], f);
        free(lines[i]);
    }
    free(lines);

    if (!has_instance)
        fprintf(f, "INSTANCE=%s\n", instance);

    fclose(f);
    return 0;
}

int main(int argc, char *argv[])
{
    /* parse global flags before command dispatch */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--plain") == 0) {
            global_plain = 1;
            global_no_color = 1;
            for (int j = i; j < argc - 1; j++) argv[j] = argv[j + 1];
            argc--; i--;
        } else if (strcmp(argv[i], "--debug") == 0) {
            global_debug = 1;
            for (int j = i; j < argc - 1; j++) argv[j] = argv[j + 1];
            argc--; i--;
        } else if (strcmp(argv[i], "--no-color") == 0) {
            global_no_color = 1;
            for (int j = i; j < argc - 1; j++) argv[j] = argv[j + 1];
            argc--; i--;
        } else if ((strcmp(argv[i], "-c") == 0 ||
                    strcmp(argv[i], "--config") == 0) && i + 1 < argc) {
            global_config_dir = argv[i + 1];
            for (int j = i; j < argc - 2; j++) argv[j] = argv[j + 2];
            argc -= 2; i--;
        }
    }

    const char *no_color_env = getenv("NO_COLOR");
    if (no_color_env && strlen(no_color_env) > 0)
        global_no_color = 1;

    static char *def_argv_man[]   = { "shyake", "man", NULL };
    static char *def_argv_check[] = { "shyake", "check", "inbox", NULL };
    static char *def_argv_count[] = { "shyake", "check", "inbox",
                                                       "--count", NULL };

    if (argc < 2) {
        char *config_dir = global_config_dir
            ? strdup(global_config_dir) : get_config_dir();
        app_config *app_cfg = read_config(config_dir);
        int def_act = app_cfg->default_action;
        free_app_config(app_cfg);
        free(config_dir);

        if (def_act == 1) {
            argv = def_argv_check;
            argc = 3;
        } else if (def_act == 2) {
            argv = def_argv_count;
            argc = 4;
        } else {
            argv = def_argv_man;
            argc = 2;
        }
    }

    const char *cmd = argv[1];

    // handle help commands
    if (strcmp(cmd, "help") == 0 || strcmp(cmd, "--help") == 0 ||
        strcmp(cmd, "-h") == 0) {
        printf("Use 'shyake man' instead.\n");
        return EXIT_SUCCESS;
    }

    // handle man command
    if (strcmp(cmd, "man") == 0) {
        if (argc < 3) {
            printf("shyake - PQC E2EE mailer\n\n");
            printf("Commands:\n");
            printf("  init          Initialize\n");
            printf("  register      Register on an instance\n");
            printf("  send          Send a piece of mail\n");
            printf("  check         Check inbox or sent\n");
            printf("  fetch         Fetch a piece of mail\n");
            printf("  burn          Burn a piece of mail\n");
            printf("  block         Block a user or instance\n");
            printf("  unblock       Unblock a user or instance\n");
            printf("  rotate        Rotate key pairs\n");
            printf("  fingerprint   Show or update fingerprint\n");
            printf("  whoami        Show current identity\n");
            printf("  destroy       Destroy identity\n");
            printf("  man           Show manual pages\n");
            printf("  version       Show client version\n\n");
            printf("Global Options:\n");
            printf("  -c, --config  Use an alternative config directory\n");
            printf("  --plain       Disable pager, color, and truncation\n");
            printf("  --no-color    Disable colored output\n");
            printf("  --debug       Output verbose curl logs to stderr\n\n");
            printf("For detailed usage, run: shyake man <command>\n");
        } else {
            const char *subcmd = argv[2];
            if (strcmp(subcmd, "init") == 0) {
                printf("shyake init - Initialize\n\n");
                printf("Usage:\n");
                printf("    shyake init\n\n");
                printf("    Generate config file and key pairs\n");
                printf("    Default directory: ~/.config/shyake/\n\n");
                printf("Options:\n");
                printf("    -c <path>       "
                       "Create multiple profiles at alternative "
                       "directories\n");
            } else if (strcmp(subcmd, "register") == 0) {
                printf("shyake register - Register on an instance\n\n");
                printf("Notes: Run this after 'shyake init'\n\n");
                printf("Usage:\n");
                printf("    shyake register -u <username> "
                       "-i <url>\n\n");
                printf("Options:\n");
                printf("    -u <username>   Your username\n");
                printf("    -i <url>        Instance URL\n");
                printf("    --debug         "
                       "Output verbose curl logs to stderr\n");
            } else if (strcmp(subcmd, "send") == 0) {
                printf("shyake send - Send a piece of mail\n\n");
                printf("Usage:\n");
                printf("    shyake send -t <username> "
                       "[-s <subject>] [<file>]\n\n");
                printf("    Read body from <file> or stdin.\n");
                printf("    First line is used as subject if -s is "
                       "omitted\n");
                printf("    (subject must not exceed 128 bytes).\n\n");
                printf("Options:\n");
                printf("    -t <username>   To (required)\n");
                printf("                    Use 'username@instance' for "
                       "an external recipient\n");
                printf("    -s <subject>    Subject line\n");
                printf("    --debug         "
                       "Output verbose curl logs to stderr\n\n");
                printf("Tips:\n");
                printf("    Send a single file:\n");
                printf("        shyake send -t salmon -s \"image.png\" "
                       "image.png\n");
                printf("    Send a directory as a tar archive:\n");
                printf("        tar czf - ./source | "
                       "shyake send -t salmon -s \"source.tar.gz\"\n");
            } else if (strcmp(subcmd, "check") == 0) {
                printf("shyake check - Check inbox or sent\n\n");
                printf("Usage:\n");
                printf("    shyake check inbox|sent\n");
                printf("    shyake check <id>\n\n");
                printf("    'check inbox' and 'check sent' list mail.\n");
                printf("    'check <id>' shows header of a piece of "
                       "mail.\n\n");
                printf("Options:\n");
                printf("    --count         "
                       "Print count only\n");
                printf("    --json          "
                       "Output as JSON\n");
                printf("    --csv           "
                       "Output as CSV\n");
                printf("    --no-header     "
                       "Disable the column header\n");
                printf("    --plain         "
                       "Disable pager, color, and truncation\n");
                printf("    --no-color      "
                       "Disable colored output\n");
                printf("    --debug         "
                       "Output verbose curl logs to stderr\n");
            } else if (strcmp(subcmd, "fetch") == 0) {
                printf("shyake fetch - Fetch and decrypt a piece of "
                       "mail\n\n");
                printf("Usage:\n");
                printf("    shyake fetch <id>\n\n");
                printf("Options:\n");
                printf("    -r, --raw       "
                       "Output body only (no header)\n");
                printf("    --plain         "
                       "Disable pager, color, and truncation\n");
                printf("    --no-color      "
                       "Disable colored output\n");
                printf("    --debug         "
                       "Output verbose curl logs to stderr\n\n");
                printf("Tips:\n");
                printf("    Save a piece of mail with header:\n");
                printf("        shyake fetch <id> --no-color "
                       "> output.txt\n");
                printf("    Save a piece of mail (body only):\n");
                printf("        shyake fetch <id> -r > output.txt\n");
                printf("    Extract a tar archive received:\n");
                printf("        shyake fetch <id> -r | tar xzf -\n");
                printf("    Extract into a specific directory:\n");
                printf("        shyake fetch <id> -r | "
                       "tar xzf - -C ./output\n");
            } else if (strcmp(subcmd, "burn") == 0) {
                printf("shyake burn - Delete a piece of mail\n\n");
                printf("Usage:\n");
                printf("    shyake burn <id>\n\n");
                printf("Options:\n");
                printf("    --debug         "
                       "Output verbose curl logs to stderr\n");
            } else if (strcmp(subcmd, "block") == 0) {
                printf("shyake block - Block a user or instance\n\n");
                printf("Usage:\n");
                printf("    shyake block <target>\n\n");
                printf("    <target> can be a username or an instance "
                       "URL.\n\n");
                printf("Options:\n");
                printf("    --debug         "
                       "Output verbose curl logs to stderr\n");
            } else if (strcmp(subcmd, "unblock") == 0) {
                printf("shyake unblock - Unblock a user or instance\n\n");
                printf("Usage:\n");
                printf("    shyake unblock <target>\n\n");
                printf("    <target> can be a username or an instance "
                       "URL.\n\n");
                printf("Options:\n");
                printf("    --debug         "
                       "Output verbose curl logs to stderr\n");
            } else if (strcmp(subcmd, "rotate") == 0) {
                printf("shyake rotate - Rotate key pairs\n\n");
                printf("Usage:\n");
                printf("    shyake rotate\n\n");
                printf("    Regenerates your key pairs and clears all "
                       "mail to and from you.\n\n");
                printf("Options:\n");
                printf("    --debug         "
                       "Output verbose curl logs to stderr\n");
            } else if (strcmp(subcmd, "fingerprint") == 0) {
                printf("shyake fingerprint - Show or update "
                       "fingerprint\n\n");
                printf("Usage:\n");
                printf("    shyake fingerprint\n");
                printf("    shyake fingerprint <username>\n");
                printf("    shyake fingerprint <username> "
                       "--update\n\n");
                printf("    Without argument: show your own "
                       "fingerprint.\n");
                printf("    With <username>: fetch and display their "
                       "public key.\n");
                printf("    --update: refresh the stored key for "
                       "<username>.\n");
                printf("    Verify the new fingerprint out-of-band "
                       "before running\n");
                printf("    --update to prevent identity "
                       "impersonation.\n\n");
                printf("Options:\n");
                printf("    --update        "
                       "Update stored key for <username>\n");
                printf("    --debug         "
                       "Output verbose curl logs to stderr\n");
            } else if (strcmp(subcmd, "whoami") == 0) {
                printf("shyake whoami - Show current identity\n\n");
                printf("Usage:\n");
                printf("    shyake whoami\n\n");
                printf("    Displays USERNAME, INSTANCE, and CONFIG "
                       "directory.\n");
            } else if (strcmp(subcmd, "destroy") == 0) {
                printf("shyake destroy - Destroy identity\n\n");
                printf("Usage:\n");
                printf("    shyake destroy\n\n");
                printf("    Deletes local config and key pairs, "
                       "destructs the account\n");
                printf("    on the instance. All mail is cleared. "
                       "The username is\n");
                printf("    permanently locked on this "
                       "instance.\n\n");
                printf("Options:\n");
                printf("    --debug         "
                       "Output verbose curl logs to stderr\n");
            } else if (strcmp(subcmd, "version") == 0) {
                printf("shyake version - Show client version\n\n");
                printf("Usage:\n");
                printf("    shyake version\n");
            } else if (strcmp(subcmd, "man") == 0) {
                printf("shyake man - Show manual pages\n\n");
                printf("Usage:\n");
                printf("    shyake man\n");
                printf("    shyake man <command>\n");
            } else {
                printf("Unknown command: %s\n", subcmd);
                printf("Run 'shyake man' for a list of available "
                       "commands.\n");
            }
        }
        return EXIT_SUCCESS;
    }

    if (strcmp(cmd, "version") == 0) {
        printf("shyake v0.1.0-rc.3\n");
        return EXIT_SUCCESS;
    }

    if (strcmp(cmd, "init") == 0) {
        int ret = cmd_init(global_config_dir);
        return ret == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
    }

    char *config_dir = global_config_dir
        ? strdup(global_config_dir) : get_config_dir();
    app_config *app_cfg = read_config(config_dir);

    if (strcmp(cmd, "whoami") == 0) {
        const char *inst = app_cfg->instance;
        if (app_cfg->username) {
            printf("USERNAME: %s\n", app_cfg->username);
        } else {
            printf("USERNAME: (not registered)\n");
        }
        printf("INSTANCE: %s\n", inst);
        printf("CONFIG:   %s\n", config_dir);
        free_app_config(app_cfg);
        free(config_dir);
        return EXIT_SUCCESS;
    }

    if (strcmp(cmd, "register") == 0) {
        char *username = NULL;
        char *instance = NULL;

        static struct option long_options[] = {
            {"username", required_argument, 0, 'u'},
            {"instance", required_argument, 0, 'i'},
            {0, 0, 0, 0}
        };

        int opt, option_index = 0;
        optind = 2;
        while ((opt = getopt_long(argc, argv, "u:i:", long_options,
                                  &option_index)) != -1) {
            switch (opt) {
                case 'u': username = optarg; break;
                case 'i': instance = optarg; break;
                default: break;
            }
        }

        if (!username) {
            fprintf(stderr, "Error: -u <username> is required "
                            "for register.\n");
            free_app_config(app_cfg);
            free(config_dir);
            return EXIT_FAILURE;
        }

        const char *inst = instance ? instance :
            (app_cfg->instance);

        
        if (!inst) {
            fprintf(stderr, "Missing INSTANCE in config file.\n");
            free_app_config(app_cfg);
            free(config_dir);
            return EXIT_FAILURE;
        }
        shyake_config cfg = {
            .config_dir = config_dir,
            .instance_url = inst,
            .username = username,
            .plain = global_plain,
            .debug = global_debug,
            .no_color = global_no_color || app_cfg->no_color
        };

        shyake_ctx *ctx = shyake_init_ctx(&cfg);
        fprintf(stderr, "Registering as %s at %s... ", username, inst);
        fflush(stderr);
        shyake_err ret = shyake_register(ctx, username);
        if (ret == SHYAKE_OK) {
            fprintf(stderr, "done.\n");
            printf("Successfully registered.\n");
            update_config_user_and_instance(config_dir, username, inst);
        } else if (ret == SHYAKE_ERR_NETWORK) {
            fprintf(stderr, "\nError: Network failure during "
                    "registration.\n");
        } else if (ret == SHYAKE_ERR_NO_INSTANCE) {
            fprintf(stderr, "\nError: Instance URL not configured.\n");
        } else {
            fprintf(stderr, "\nError: Registration failed "
                    "(server rejected).\n");
        }

        shyake_free_ctx(ctx);
        free_app_config(app_cfg);
        free(config_dir);
        return ret == SHYAKE_OK ? EXIT_SUCCESS : EXIT_FAILURE;
    }

    if (strcmp(cmd, "send") == 0) {
        char *recipient = NULL;
        char *subject = NULL;

        static struct option long_options[] = {
            {"to", required_argument, 0, 't'},
            {"subject", required_argument, 0, 's'},
            {0, 0, 0, 0}
        };

        int opt, option_index = 0;
        optind = 2;
        while ((opt = getopt_long(argc, argv, "t:s:", long_options,
                                  &option_index)) != -1) {
            switch (opt) {
                case 't': recipient = optarg; break;
                case 's': subject = optarg; break;
                default: break;
            }
        }

        if (!recipient) {
            fprintf(stderr, "Error: -t <recipient> is required "
                            "for send.\n");
            free_app_config(app_cfg);
            free(config_dir);
            return EXIT_FAILURE;
        }

        if (subject && strlen(subject) > 128) {
            fprintf(stderr, "Error: Subject cannot exceed 128 bytes.\n");
            free_app_config(app_cfg);
            free(config_dir);
            return EXIT_FAILURE;
        }

        FILE *in_file = stdin;
        if (optind < argc) {
            in_file = fopen(argv[optind], "rb");
            if (!in_file) {
                fprintf(stderr, "Failed to open file: %s\n", argv[optind]);
                free_app_config(app_cfg);
                free(config_dir);
                return EXIT_FAILURE;
            }
        }

        size_t body_len;
        uint8_t *body = read_all_bytes(in_file, &body_len);
        if (in_file != stdin)
            fclose(in_file);

        if (!body) {
            fprintf(stderr, "Failed to read body.\n");
            free_app_config(app_cfg);
            free(config_dir);
            return EXIT_FAILURE;
        }

        const char *inst = app_cfg->instance;
        const char *user = app_cfg->username;

        
        if (!inst || !user) {
            fprintf(stderr, 
                    "Missing INSTANCE or USERNAME in config file.\n");
            free_app_config(app_cfg);
            free(config_dir);
            return EXIT_FAILURE;
        }
        shyake_config cfg = {
            .config_dir = config_dir,
            .instance_url = inst,
            .username = user,
            .plain = global_plain,
            .debug = global_debug,
            .no_color = global_no_color || app_cfg->no_color
        };

        shyake_ctx *ctx = shyake_init_ctx(&cfg);
        fprintf(stderr, "Sending mail to %s... ", recipient);
        fflush(stderr);
        shyake_err ret = shyake_send(ctx, recipient, subject, body,
                                    body_len);
        if (ret == SHYAKE_OK) {
            fprintf(stderr, "done.\n");
            printf("Your mail was sent.\n");
        } else if (ret == SHYAKE_ERR_KEY_MISMATCH) {
            fprintf(stderr, "\n\nFATAL: Remote public key of recipient "
                    "has changed!\n"
                    "RUN 'shyake fingerprint <username>' to "
                    "inspect and update trust.\n");
        } else if (ret == SHYAKE_ERR_GONE) {
            fprintf(stderr, "\n\nFATAL: Recipient no longer exists.\n");
        } else if (ret == SHYAKE_ERR_NETWORK) {
            fprintf(stderr, "\nError: Network failure.\n");
        } else if (ret == SHYAKE_ERR_CRYPTO) {
            fprintf(stderr, "\nError: Cryptographic operation failed.\n");
        } else {
            fprintf(stderr, "\nError: Send failed.\n");
        }

        shyake_free_ctx(ctx);
        free_app_config(app_cfg);
        free(config_dir);
        free(body);
        return ret == SHYAKE_OK ? EXIT_SUCCESS : EXIT_FAILURE;
    }

    if (strcmp(cmd, "check") == 0) {
        if (argc < 3) {
            fprintf(stderr,
                    "Usage: shyake check <inbox|sent|id> [options]\n");
            free_app_config(app_cfg);
            free(config_dir);
            return EXIT_FAILURE;
        }

        const char *arg = argv[2];
        int is_list = (strcmp(arg, "inbox") == 0 ||
                       strcmp(arg, "sent") == 0);

        cli_render_opts ro = {0};
        if (is_list) {
            static struct option check_options[] = {
                {"count", no_argument, 0, 'C'},
                {"json", no_argument, 0, 'J'},
                {"csv", no_argument, 0, 'S'},
                {"no-header", no_argument, 0, 'H'},
                {0, 0, 0, 0}
            };
            int opt, opt_idx = 0;
            optind = 3;
            while ((opt = getopt_long(argc, argv, "", check_options,
                                      &opt_idx)) != -1) {
                switch (opt) {
                    case 'C': ro.count_only = 1; break;
                    case 'J': ro.json_out = 1; break;
                    case 'S': ro.csv_out = 1; break;
                    case 'H': ro.no_header = 1; break;
                    default: break;
                }
            }
        }

        const char *inst = app_cfg->instance;
        const char *user = app_cfg->username;

        if (!inst || !user) {
            fprintf(stderr,
                    "Missing INSTANCE or USERNAME in config file.\n");
            free_app_config(app_cfg);
            free(config_dir);
            return EXIT_FAILURE;
        }
        shyake_config cfg = {
            .config_dir = config_dir,
            .instance_url = inst,
            .username = user,
            .plain = global_plain,
            .debug = global_debug,
            .no_color = global_no_color || app_cfg->no_color
        };

        shyake_ctx *ctx = shyake_init_ctx(&cfg);
        int ret = 0;

        if (is_list) {
            ro.no_color        = cfg.no_color;
            ro.plain           = cfg.plain;
            ro.tz_hours        = app_cfg->tz_hours;
            ro.time_fmt        = app_cfg->time_format;
            ro.time_fmt_recent = app_cfg->time_format_recent;
            parse_check_columns(app_cfg->check_columns,
                                ro.col_order, &ro.col_count);
            shyake_mail_list *list = shyake_check(ctx, arg);
            if (list) {
                cli_render_mail_list(list, &ro);
                shyake_free_mail_list(list);
            } else {
                ret = -1;
            }
        } else {
            /* check <id> metadata view */
            shyake_mail_detail *d = shyake_check_one(ctx, arg);
            if (d) {
                cli_render_mail_header(d, cfg.no_color,
                                       app_cfg->tz_hours,
                                       app_cfg->time_format,
                                       app_cfg->time_format_recent);
                shyake_free_mail_detail(d);
            } else {
                ret = -1;
            }
        }

        shyake_free_ctx(ctx);
        free_app_config(app_cfg);
        free(config_dir);
        return ret == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
    }

    if (strcmp(cmd, "fetch") == 0) {
        int raw = 0;
        const char *mail_id = NULL;

        static struct option long_options[] = {
            {"raw", no_argument, 0, 'r'},
            {0, 0, 0, 0}
        };

        int opt, option_index = 0;
        optind = 2;
        while ((opt = getopt_long(argc, argv, "r", long_options,
                                  &option_index)) != -1) {
            switch (opt) {
                case 'r': raw = 1; break;
                default: break;
            }
        }

        if (optind < argc) {
            mail_id = argv[optind];
        }

        if (!mail_id) {
            fprintf(stderr, "Error: Mail ID is required for fetch.\n");
            free_app_config(app_cfg);
            free(config_dir);
            return EXIT_FAILURE;
        }

        const char *inst = app_cfg->instance;
        const char *user = app_cfg->username;

        if (!inst || !user) {
            fprintf(stderr,
                    "Missing INSTANCE or USERNAME in config file.\n");
            free_app_config(app_cfg);
            free(config_dir);
            return EXIT_FAILURE;
        }
        shyake_config cfg = {
            .config_dir = config_dir,
            .instance_url = inst,
            .username = user,
            .plain = global_plain,
            .debug = global_debug,
            .no_color = global_no_color || app_cfg->no_color
        };

        shyake_ctx *ctx = shyake_init_ctx(&cfg);
        int ret = 0;
        shyake_mail_detail *d = shyake_fetch(ctx, mail_id);
        if (d) {
            cli_render_mail_detail(d, raw, cfg.no_color, cfg.plain,
                                   app_cfg->tz_hours,
                                   app_cfg->time_format,
                                   app_cfg->time_format_recent);
            shyake_free_mail_detail(d);
        } else {
            ret = -1;
        }

        shyake_free_ctx(ctx);
        free_app_config(app_cfg);
        free(config_dir);
        return ret == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
    }

    if (strcmp(cmd, "burn") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Usage: shyake burn <id>\n");
            free_app_config(app_cfg);
            free(config_dir);
            return EXIT_FAILURE;
        }
        const char *mail_id = argv[2];
        const char *inst = app_cfg->instance;
        const char *user = app_cfg->username;

        
        if (!inst || !user) {
            fprintf(stderr,
                    "Missing INSTANCE or USERNAME in config file.\n");
            free_app_config(app_cfg);
            free(config_dir);
            return EXIT_FAILURE;
        }
        shyake_config cfg = {
            .config_dir = config_dir,
            .instance_url = inst,
            .username = user,
            .plain = global_plain,
            .debug = global_debug,
            .no_color = global_no_color || app_cfg->no_color
        };
        shyake_ctx *ctx = shyake_init_ctx(&cfg);
        shyake_err ret = shyake_burn(ctx, mail_id);
        shyake_free_ctx(ctx);
        free_app_config(app_cfg);
        free(config_dir);
        if (ret == SHYAKE_OK)
            printf("Mail burned.\n");
        else if (ret == SHYAKE_ERR_NOT_FOUND)
            fprintf(stderr, "Error: Mail not found.\n");
        else if (ret == SHYAKE_ERR_FORBIDDEN)
            fprintf(stderr, "Error: Permission denied.\n");
        else if (ret == SHYAKE_ERR_NETWORK)
            fprintf(stderr, "Error: Network failure.\n");
        else
            fprintf(stderr, "Error: Burn failed.\n");
        return ret == SHYAKE_OK ? EXIT_SUCCESS : EXIT_FAILURE;
    }

    if (strcmp(cmd, "block") == 0 || strcmp(cmd, "unblock") == 0) {
        int is_unblock = strcmp(cmd, "unblock") == 0;
        if (argc < 3) {
            fprintf(stderr, "Usage: shyake %s <target>\n", cmd);
            free_app_config(app_cfg);
            free(config_dir);
            return EXIT_FAILURE;
        }
        const char *target = argv[2];
        const char *inst = app_cfg->instance;
        const char *user = app_cfg->username;

        
        if (!inst || !user) {
            fprintf(stderr,
                    "Missing INSTANCE or USERNAME in config file.\n");
            free_app_config(app_cfg);
            free(config_dir);
            return EXIT_FAILURE;
        }
        shyake_config cfg = {
            .config_dir = config_dir,
            .instance_url = inst,
            .username = user,
            .plain = global_plain,
            .debug = global_debug,
            .no_color = global_no_color || app_cfg->no_color
        };
        shyake_ctx *ctx = shyake_init_ctx(&cfg);
        shyake_err ret = shyake_block(ctx, target, is_unblock);
        shyake_free_ctx(ctx);
        free_app_config(app_cfg);
        free(config_dir);
        if (ret == SHYAKE_OK)
            printf("%s %s.\n", target,
                   is_unblock ? "unblocked" : "blocked");
        else if (ret == SHYAKE_ERR_NETWORK)
            fprintf(stderr, "Error: Network failure.\n");
        else
            fprintf(stderr, "Error: Operation failed.\n");
        return ret == SHYAKE_OK ? EXIT_SUCCESS : EXIT_FAILURE;
    }

    if (strcmp(cmd, "rotate") == 0) {
        const char *inst = app_cfg->instance;
        const char *user = app_cfg->username;

        
        if (!inst || !user) {
            fprintf(stderr,
                    "Missing INSTANCE or USERNAME in config file.\n");
            free_app_config(app_cfg);
            free(config_dir);
            return EXIT_FAILURE;
        }
        shyake_config cfg = {
            .config_dir = config_dir,
            .instance_url = inst,
            .username = user,
            .plain = global_plain,
            .debug = global_debug,
            .no_color = global_no_color || app_cfg->no_color
        };

        shyake_ctx *ctx = shyake_init_ctx(&cfg);
        fprintf(stderr, "Rotating keys for %s... ", user);
        fflush(stderr);
        shyake_err ret = shyake_rotate(ctx);
        shyake_free_ctx(ctx);
        free_app_config(app_cfg);
        free(config_dir);
        if (ret == SHYAKE_OK) {
            fprintf(stderr, "done.\n");
            printf("Keys successfully rotated.\n");
        } else if (ret == SHYAKE_ERR_NETWORK) {
            fprintf(stderr, "\nError: Network failure.\n");
        } else if (ret == SHYAKE_ERR_CRYPTO) {
            fprintf(stderr, "\nError: Key generation failed.\n");
        } else {
            fprintf(stderr, "\nError: Rotation failed.\n");
        }
        return ret == SHYAKE_OK ? EXIT_SUCCESS : EXIT_FAILURE;
    }

    if (strcmp(cmd, "fingerprint") == 0) {
        int do_update = 0;
        const char *target_user = NULL;
        for (int i = 2; i < argc; i++) {
            if (strcmp(argv[i], "--update") == 0) {
                do_update = 1;
            } else if (!target_user) {
                target_user = argv[i];
            }
        }
        
        const char *inst = app_cfg->instance;
        const char *user = app_cfg->username;

        
        if (!inst || !user) {
            fprintf(stderr,
                    "Missing INSTANCE or USERNAME in config file.\n");
            free_app_config(app_cfg);
            free(config_dir);
            return EXIT_FAILURE;
        }
        shyake_config cfg = {
            .config_dir = config_dir,
            .instance_url = inst,
            .username = user,
            .plain = global_plain,
            .debug = global_debug,
            .no_color = global_no_color || app_cfg->no_color
        };

        shyake_ctx *ctx = shyake_init_ctx(&cfg);
        int ret = 0;
        int is_self = (target_user == NULL);

        if (is_self)
            printf("Fingerprint for %s (local):\n", user);
        else if (!do_update)
            printf("Fetching public key for %s...\n", target_user);

        shyake_fp_result *fp = shyake_fingerprint(
            ctx, target_user, do_update);
        if (fp) {
            if (!is_self && do_update) {
                /* --update: only confirm, no art */
                printf("Successfully updated known_hosts for %s.\n",
                       target_user);
            } else {
                cli_render_fingerprint(
                    is_self ? user : target_user, fp, is_self);
            }
            shyake_free_fp_result(fp);
        } else {
            fprintf(stderr, "Failed to fetch public key.\n");
            ret = -1;
        }

        shyake_free_ctx(ctx);
        free_app_config(app_cfg);
        free(config_dir);
        return ret == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
    }

    if (strcmp(cmd, "destroy") == 0) {
        const char *inst = app_cfg->instance;
        const char *user = app_cfg->username;

        printf("WARNING: This will delete your local configurations "
               "and key pairs, also\n");
        printf("destruct your account on the instance. All mail to and"
               " from you will be\n");
        printf("cleared. Your username will be permanently locked and"
               " unregisterable on\n");
        printf("this instance. Type your username to confirm: ");
        fflush(stdout);

        char buf[256];
        if (!fgets(buf, sizeof(buf), stdin)) {
            free_app_config(app_cfg);
            free(config_dir);
            return EXIT_FAILURE;
        }

        char *input = trim_whitespace(buf);
        if (strcmp(input, user) != 0) {
            fprintf(stderr, "Username mismatch. Aborted.\n");
            free_app_config(app_cfg);
            free(config_dir);
            return EXIT_FAILURE;
        }

        
        if (!inst || !user) {
            fprintf(stderr,
                    "Missing INSTANCE or USERNAME in config file.\n");
            free_app_config(app_cfg);
            free(config_dir);
            return EXIT_FAILURE;
        }
        shyake_config cfg = {
            .config_dir = config_dir,
            .instance_url = inst,
            .username = user,
            .plain = global_plain,
            .debug = global_debug,
            .no_color = global_no_color || app_cfg->no_color
        };

        shyake_ctx *ctx = shyake_init_ctx(&cfg);
        shyake_err ret = shyake_destroy(ctx);

        shyake_free_ctx(ctx);
        free_app_config(app_cfg);

        if (ret == SHYAKE_OK) {
            char cmd_buf[512];
            snprintf(cmd_buf, sizeof(cmd_buf), "rm -rf %s/*", config_dir);
            system(cmd_buf);
            printf("Account destroyed. "
                   "Local configuration and keys deleted.\n");
        } else if (ret == SHYAKE_ERR_NETWORK) {
            fprintf(stderr, "Error: Network failure.\n");
        } else {
            fprintf(stderr, "Error: Destroy failed.\n");
        }

        free(config_dir);
        return ret == SHYAKE_OK ? EXIT_SUCCESS : EXIT_FAILURE;
    }

    fprintf(stderr, "Unknown command: %s\n", cmd);
    free_app_config(app_cfg);
    free(config_dir);
    return EXIT_FAILURE;
}
