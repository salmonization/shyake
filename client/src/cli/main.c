#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include "shyake.h"

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
        }
    }
    fclose(f);
    return cfg;
}



static int
update_config_user_and_instance(const char *config_dir, const char *username, const char *instance)
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
            snprintf(new_line, sizeof(new_line), "INSTANCE=%s\nUSERNAME=%s\n", instance, username);
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

    if (argc < 2) {
        fprintf(stderr, "Usage: shyake <command> [args...]\n");
        return EXIT_FAILURE;
    }

    const char *cmd = argv[1];

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
        int ret = shyake_register(ctx, username);
        if (ret == 0) {
            
            update_config_user_and_instance(config_dir, username, inst);
        }

        shyake_free_ctx(ctx);
        free_app_config(app_cfg);
        free(config_dir);
        return ret == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
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
            fprintf(stderr, "Failed to read message body.\n");
            free_app_config(app_cfg);
            free(config_dir);
            return EXIT_FAILURE;
        }

        const char *inst = app_cfg->instance;
        const char *user = app_cfg->username;

        
        if (!inst || !user) {
            fprintf(stderr, "Missing INSTANCE or USERNAME in config file.\n");
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
        int ret = shyake_send(ctx, recipient, subject, body, body_len);

        shyake_free_ctx(ctx);
        free_app_config(app_cfg);
        free(config_dir);
        free(body);
        return ret == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
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

        shyake_check_opts chk_opts = {0};
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
                    case 'C': chk_opts.count_only = 1; break;
                    case 'J': chk_opts.json_out = 1; break;
                    case 'S': chk_opts.csv_out = 1; break;
                    case 'H': chk_opts.no_header = 1; break;
                    default: break;
                }
            }
        }

        const char *inst = app_cfg->instance;
        const char *user = app_cfg->username;

        
        if (!inst || !user) {
            fprintf(stderr, "Missing INSTANCE or USERNAME in config file.\n");
            free_app_config(app_cfg);
            free(config_dir);
            return EXIT_FAILURE;
        }
        shyake_config cfg = {
            .config_dir = config_dir,
            .instance_url = inst,
            .username = user,
            .time_format = app_cfg->time_format,
            .time_format_recent = app_cfg->time_format_recent,
            .check_columns = app_cfg->check_columns,
            .plain = global_plain,
            .debug = global_debug,
            .no_color = global_no_color || app_cfg->no_color
        };

        shyake_ctx *ctx = shyake_init_ctx(&cfg);
        int ret;
        if (is_list) {
            ret = shyake_check(ctx, arg, &chk_opts);
        } else {
            /* check <id> directly */
            ret = shyake_check_one(ctx, arg);
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
            fprintf(stderr, "Error: mail_id is required for fetch.\n");
            free_app_config(app_cfg);
            free(config_dir);
            return EXIT_FAILURE;
        }

        const char *inst = app_cfg->instance;
        const char *user = app_cfg->username;

        
        if (!inst || !user) {
            fprintf(stderr, "Missing INSTANCE or USERNAME in config file.\n");
            free_app_config(app_cfg);
            free(config_dir);
            return EXIT_FAILURE;
        }
        shyake_config cfg = {
            .config_dir = config_dir,
            .instance_url = inst,
            .username = user,
            .time_format = app_cfg->time_format,
            .time_format_recent = app_cfg->time_format_recent,
            .check_columns = app_cfg->check_columns,
            .plain = global_plain,
            .debug = global_debug,
            .no_color = global_no_color || app_cfg->no_color
        };

        shyake_ctx *ctx = shyake_init_ctx(&cfg);
        int ret = shyake_fetch(ctx, mail_id, raw);

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
            fprintf(stderr, "Missing INSTANCE or USERNAME in config file.\n");
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
        int ret = shyake_burn(ctx, mail_id);
        shyake_free_ctx(ctx);
        free_app_config(app_cfg);
        free(config_dir);
        return ret == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
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
            fprintf(stderr, "Missing INSTANCE or USERNAME in config file.\n");
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
        int ret = shyake_block(ctx, target, is_unblock);
        shyake_free_ctx(ctx);
        free_app_config(app_cfg);
        free(config_dir);
        return ret == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
    }

    if (strcmp(cmd, "rotate") == 0) {
        const char *inst = app_cfg->instance;
        const char *user = app_cfg->username;

        
        if (!inst || !user) {
            fprintf(stderr, "Missing INSTANCE or USERNAME in config file.\n");
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
        int ret = shyake_rotate(ctx);

        shyake_free_ctx(ctx);
        free_app_config(app_cfg);
        free(config_dir);
        return ret == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
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
            fprintf(stderr, "Missing INSTANCE or USERNAME in config file.\n");
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
        int ret = shyake_fingerprint(ctx, target_user, do_update);

        shyake_free_ctx(ctx);
        free_app_config(app_cfg);
        free(config_dir);
        return ret == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
    }

    if (strcmp(cmd, "destroy") == 0) {
        const char *inst = app_cfg->instance;
        const char *user = app_cfg->username;

        printf("WARNING: This will permanently delete your KEYS and ALL "
               "MESSAGES sent to or\n");
        printf("from you. And your username will be locked forever and "
               "cannot be registered\n");
        printf("again. Type your username to confirm: ");
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
            fprintf(stderr, "Missing INSTANCE or USERNAME in config file.\n");
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
        int ret = shyake_destroy(ctx);

        shyake_free_ctx(ctx);
        free_app_config(app_cfg);

        if (ret == 0) {
            char cmd_buf[512];
            snprintf(cmd_buf, sizeof(cmd_buf), "rm -rf %s/*", config_dir);
            system(cmd_buf);
            printf("Local configuration and keys securely deleted.\n");
        }

        free(config_dir);
        return ret == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
    }

    fprintf(stderr, "Unknown command: %s\n", cmd);
    free_app_config(app_cfg);
    free(config_dir);
    return EXIT_FAILURE;
}
