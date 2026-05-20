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

static char*
read_username(const char *config_dir)
{
    // read registered username from dedicated file
    char path[512];
    snprintf(path, sizeof(path), "%s/username", config_dir);
    FILE *f = fopen(path, "r");
    if (!f)
        return NULL;

    char buf[256];
    if (!fgets(buf, sizeof(buf), f)) {
        fclose(f);
        return NULL;
    }
    fclose(f);
    return strdup(trim_whitespace(buf));
}

static int
write_username(const char *config_dir, const char *username)
{
    // persist username to dedicated file
    char path[512];
    snprintf(path, sizeof(path), "%s/username", config_dir);
    FILE *f = fopen(path, "w");
    if (!f)
        return -1;
    fprintf(f, "%s\n", username);
    fclose(f);
    return 0;
}

static int
update_config_instance(const char *config_dir, const char *instance)
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
            snprintf(new_line, sizeof(new_line), "INSTANCE=%s\n", instance);
            lines[i] = strdup(new_line);
            has_instance = 1;
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
    char *stored_username = read_username(config_dir);

    if (strcmp(cmd, "whoami") == 0) {
        const char *inst = app_cfg->instance
            ? app_cfg->instance : "https://shyake.eee.coffee";
        if (stored_username) {
            printf("Username: %s\n", stored_username);
        } else {
            printf("Username: (not registered)\n");
        }
        printf("Instance: %s\n", inst);
        printf("Config:   %s\n", config_dir);
        free(stored_username);
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
            fprintf(stderr, "Error: -u <username> is required for register.\n");
            free(stored_username);
            free_app_config(app_cfg);
            free(config_dir);
            return EXIT_FAILURE;
        }

        const char *inst = instance ? instance :
            (app_cfg->instance ? app_cfg->instance
                               : "https://shyake.eee.coffee");

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
            write_username(config_dir, username);
            update_config_instance(config_dir, inst);
        }

        shyake_free_ctx(ctx);
        free(stored_username);
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
            fprintf(stderr, "Error: -t <recipient> is required for send.\n");
            free(stored_username);
            free_app_config(app_cfg);
            free(config_dir);
            return EXIT_FAILURE;
        }

        if (subject && strlen(subject) > 128) {
            fprintf(stderr, "Error: Subject cannot exceed 128 bytes.\n");
            free(stored_username);
            free_app_config(app_cfg);
            free(config_dir);
            return EXIT_FAILURE;
        }

        FILE *in_file = stdin;
        if (optind < argc) {
            in_file = fopen(argv[optind], "rb");
            if (!in_file) {
                fprintf(stderr, "Failed to open file: %s\n", argv[optind]);
                free(stored_username);
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
            free(stored_username);
            free_app_config(app_cfg);
            free(config_dir);
            return EXIT_FAILURE;
        }

        const char *inst = app_cfg->instance
            ? app_cfg->instance : "https://shyake.eee.coffee";
        const char *user = stored_username ? stored_username : "salmon";

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
        free(stored_username);
        free_app_config(app_cfg);
        free(config_dir);
        free(body);
        return ret == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
    }

    if (strcmp(cmd, "check") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Usage: shyake check <inbox|sent>\n");
            free(stored_username);
            free_app_config(app_cfg);
            free(config_dir);
            return EXIT_FAILURE;
        }
        const char *type = argv[2];
        if (strcmp(type, "inbox") != 0 && strcmp(type, "sent") != 0) {
            fprintf(stderr, "Type must be inbox or sent.\n");
            free(stored_username);
            free_app_config(app_cfg);
            free(config_dir);
            return EXIT_FAILURE;
        }

        const char *inst = app_cfg->instance
            ? app_cfg->instance : "https://shyake.eee.coffee";
        const char *user = stored_username ? stored_username : "salmon";

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
        int ret = shyake_check(ctx, type);

        shyake_free_ctx(ctx);
        free(stored_username);
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
            free(stored_username);
            free_app_config(app_cfg);
            free(config_dir);
            return EXIT_FAILURE;
        }

        const char *inst = app_cfg->instance
            ? app_cfg->instance : "https://shyake.eee.coffee";
        const char *user = stored_username ? stored_username : "salmon";

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
        free(stored_username);
        free_app_config(app_cfg);
        free(config_dir);
        return ret == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
    }

    fprintf(stderr, "Unknown command: %s\n", cmd);
    free(stored_username);
    free_app_config(app_cfg);
    free(config_dir);
    return EXIT_FAILURE;
}
