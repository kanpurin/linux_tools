#ifndef LTREE_H
#define LTREE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/types.h>

typedef enum {
    SORT_NAME,
    SORT_TIME,
    SORT_SIZE,
    SORT_NONE
} SortKey;

typedef struct {
    bool all;
    bool dirs_only;
    bool directory_as_file;
    bool full_path;
    bool long_format;
    bool human_size;
    bool numeric_ids;
    bool du;
    bool classify;
    bool slash_directories;
    bool one_filesystem;
    bool dirs_first;
    bool reverse;
    bool ascii;
    bool report;
    int color; /* -1: auto, 0: never, 1: always */
    long max_depth;
    SortKey sort_key;
} Options;

typedef struct {
    unsigned long directories;
    unsigned long files;
    unsigned long errors;
} Totals;

extern Options options;

int parse_options(int argc, char **argv);
void initialize_output(void);
void print_metadata(const struct stat *st, bool stat_ok, uintmax_t displayed_size);
void print_name(const char *shown, const char *path, const struct stat *st, bool stat_ok);
void print_prefix(const bool *last_at_depth, size_t depth, bool is_last);
void measure_stat_columns(const struct stat *st, uintmax_t displayed_size);
void measure_directory_columns(const char *path, long depth, dev_t root_device);
void walk_directory(const char *path, long depth, dev_t root_device,
                    bool *last_at_depth, Totals *totals);
uintmax_t displayed_size(const char *path, const struct stat *st, dev_t root_device);
void free_size_cache(void);

#endif
