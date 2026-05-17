#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "shyake.h"

int main(int argc, char *argv[])
{
    if (argc < 2) {
        fprintf(stderr, "Usage: shyake <command> [args...]\n");
        return EXIT_FAILURE;
    }

    if (strcmp(argv[1], "init") == 0) {
        return cmd_init() == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
    } else {
        fprintf(stderr, "Unknown command: %s\n", argv[1]);
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
