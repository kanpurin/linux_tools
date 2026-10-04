#include "ltree.h"

#include <errno.h>
#include <limits.h>
#include <locale.h>
#include <stdio.h>
#include <string.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

int main(int argc, char **argv)
{
    Totals totals = {0};
    bool last_at_depth[PATH_MAX] = {false};

    setlocale(LC_ALL, "");
    int first_path = parse_options(argc, argv);
    initialize_output();

    int path_count = argc - first_path;
    if (path_count == 0)
        path_count = 1;

    if (options.long_format) {
        for (int i = 0; i < path_count; i++) {
            const char *path = argc == first_path ? "." : argv[first_path + i];
            struct stat st;
            uintmax_t size;

            if (lstat(path, &st) != 0)
                continue;
            size = displayed_size(path, &st, st.st_dev);
            measure_stat_columns(&st, size);
            if (S_ISDIR(st.st_mode) && !options.directory_as_file)
                measure_directory_columns(path, 0, st.st_dev);
        }
    }

    for (int i = 0; i < path_count; i++) {
        const char *path = argc == first_path ? "." : argv[first_path + i];
        struct stat st;
        uintmax_t size;

        if (i > 0)
            putchar('\n');
        if (lstat(path, &st) != 0) {
            fprintf(stderr, "ltree: %s: %s\n", path, strerror(errno));
            totals.errors++;
            continue;
        }
        size = displayed_size(path, &st, st.st_dev);
        print_metadata(&st, true, size);
        print_name(path, path, &st, true);
        putchar('\n');

        if (S_ISDIR(st.st_mode) && !options.directory_as_file) {
            walk_directory(path, 0, st.st_dev, last_at_depth, &totals);
        } else if (!options.dirs_only) {
            totals.files++;
        }
    }

    if (options.report) {
        printf("\n%lu director%s", totals.directories,
               totals.directories == 1 ? "y" : "ies");
        if (!options.dirs_only)
            printf(", %lu file%s", totals.files, totals.files == 1 ? "" : "s");
        putchar('\n');
    }
    free_size_cache();
    return totals.errors ? 1 : 0;
}
