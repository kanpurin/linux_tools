#include "ltree.h"

#include <errno.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VERSION "2.3.1"

Options options = {
    .report = true,
    .color = -1,
    .max_depth = 1,
    .sort_key = SORT_NAME,
};
static void usage(FILE *out)
{
    fprintf(out,
            "Usage: ltree [OPTION]... [DIRECTORY]...\n"
            "Display directories as trees.\n\n"
            "  -a, --all              include hidden entries\n"
            "  -d, --directory        list directories themselves, not their contents\n"
            "  -f                     include hidden entries and do not sort\n"
            "  -l                     use a long listing format\n"
            "  -h, --human-readable   print human-readable sizes with -l\n"
            "  -n, --numeric-uid-gid  like -l, but show numeric user and group IDs\n"
            "  -F, --classify         append an indicator (one of */=>@|)\n"
            "  -p                      append / to directories\n"
            "  -t                     sort by modification time, newest first\n"
            "  -S                     sort by size, largest first\n"
            "  -r, --reverse          reverse the sorting order\n"
            "  -R, --recursive        descend without a depth limit\n"
            "  -L, --depth N          descend at most N levels\n"
            "      --dirs-only        show directories only\n"
            "      --dirs-first       list directories before files\n"
            "      --full-path        print the full path of each entry\n"
            "      --one-file-system  stay on the starting filesystem\n"
            "      --du               show the total size of each directory tree\n"
            "      --color[=WHEN]     colorize: always, auto, or never\n"
            "      --ascii            use ASCII branch characters\n"
            "      --unicode          use Unicode box-drawing branches (default)\n"
            "      --no-summary       omit the file/directory report\n"
            "      --help         display this help and exit\n"
            "      --version      output version information and exit\n");
}

static long parse_depth(const char *text)
{
    char *end = NULL;
    errno = 0;
    long value = strtol(text, &end, 10);
    if (errno || end == text || *end != '\0' || value <= 0) {
        fprintf(stderr, "ltree: invalid level '%s'\n", text);
        exit(2);
    }
    return value;
}

static int parse_color(const char *when)
{
    if (when == NULL || !strcmp(when, "always"))
        return 1;
    if (!strcmp(when, "auto"))
        return -1;
    if (!strcmp(when, "never"))
        return 0;
    fprintf(stderr, "ltree: invalid color value '%s'\n", when);
    exit(2);
}

int parse_options(int argc, char **argv)
{
    enum {
        OPT_DIRS_ONLY = 256,
        OPT_DIRSFIRST,
        OPT_FULL_PATH,
        OPT_ONE_FILE_SYSTEM,
        OPT_DU,
        OPT_COLOR,
        OPT_ASCII,
        OPT_UNICODE,
        OPT_NO_SUMMARY,
        OPT_HELP,
        OPT_VERSION
    };
    static const struct option long_options[] = {
        {"all", no_argument, NULL, 'a'},
        {"directory", no_argument, NULL, 'd'},
        {"human-readable", no_argument, NULL, 'h'},
        {"numeric-uid-gid", no_argument, NULL, 'n'},
        {"classify", no_argument, NULL, 'F'},
        {"reverse", no_argument, NULL, 'r'},
        {"recursive", no_argument, NULL, 'R'},
        {"depth", required_argument, NULL, 'L'},
        {"dirs-only", no_argument, NULL, OPT_DIRS_ONLY},
        {"dirs-first", no_argument, NULL, OPT_DIRSFIRST},
        {"full-path", no_argument, NULL, OPT_FULL_PATH},
        {"one-file-system", no_argument, NULL, OPT_ONE_FILE_SYSTEM},
        {"du", no_argument, NULL, OPT_DU},
        {"color", optional_argument, NULL, OPT_COLOR},
        {"ascii", no_argument, NULL, OPT_ASCII},
        {"unicode", no_argument, NULL, OPT_UNICODE},
        {"no-summary", no_argument, NULL, OPT_NO_SUMMARY},
        {"help", no_argument, NULL, OPT_HELP},
        {"version", no_argument, NULL, OPT_VERSION},
        {NULL, 0, NULL, 0}
    };
    int option;
    while ((option = getopt_long(argc, argv, "adfFhL:lnprRtSV", long_options, NULL)) != -1) {
        switch (option) {
        case 'a': options.all = true; break;
        case 'd': options.directory_as_file = true; break;
        case 'f': options.all = true; options.sort_key = SORT_NONE; break;
        case 'F': options.classify = true; break;
        case 'h': options.human_size = true; break;
        case 'L': options.max_depth = parse_depth(optarg); break;
        case 'l': options.long_format = true; break;
        case 'n': options.long_format = true; options.numeric_ids = true; break;
        case 'p': options.slash_directories = true; break;
        case 'r': options.reverse = true; break;
        case 'R': options.max_depth = -1; break;
        case 't': options.sort_key = SORT_TIME; break;
        case 'S': options.sort_key = SORT_SIZE; break;
        case 'V': printf("ltree %s\n", VERSION); exit(0);
        case OPT_DIRS_ONLY: options.dirs_only = true; break;
        case OPT_DIRSFIRST: options.dirs_first = true; break;
        case OPT_FULL_PATH: options.full_path = true; break;
        case OPT_ONE_FILE_SYSTEM: options.one_filesystem = true; break;
        case OPT_DU: options.du = true; options.long_format = true; break;
        case OPT_COLOR: options.color = parse_color(optarg); break;
        case OPT_ASCII: options.ascii = true; break;
        case OPT_UNICODE: options.ascii = false; break;
        case OPT_NO_SUMMARY: options.report = false; break;
        case OPT_HELP: usage(stdout); exit(0);
        case OPT_VERSION: printf("ltree %s\n", VERSION); exit(0);
        default: usage(stderr); exit(2);
        }
    }

    return optind;
}
