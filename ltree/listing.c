#include "ltree.h"

#include <dirent.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SIZE_CACHE_BUCKETS 4099

typedef struct {
    char *name;
    char *path;
    struct stat st;
    bool stat_ok;
} Entry;

typedef struct SizeCache {
    dev_t device;
    ino_t inode;
    uintmax_t size;
    struct SizeCache *next;
} SizeCache;

static SizeCache *size_cache[SIZE_CACHE_BUCKETS];

static void *xmalloc(size_t size)
{
    void *ptr = malloc(size);
    if (ptr == NULL) {
        fprintf(stderr, "ltree: out of memory\n");
        exit(2);
    }
    return ptr;
}

static char *xstrdup(const char *s)
{
    char *copy = strdup(s);
    if (copy == NULL) {
        fprintf(stderr, "ltree: out of memory\n");
        exit(2);
    }
    return copy;
}

static char *join_path(const char *left, const char *right)
{
    size_t left_len = strlen(left);
    size_t right_len = strlen(right);
    bool slash = left_len > 0 && left[left_len - 1] != '/';
    char *path = xmalloc(left_len + (size_t)slash + right_len + 1);

    memcpy(path, left, left_len);
    if (slash)
        path[left_len++] = '/';
    memcpy(path + left_len, right, right_len + 1);
    return path;
}

static int entry_compare(const void *left_ptr, const void *right_ptr)
{
    const Entry *left = left_ptr;
    const Entry *right = right_ptr;
    int result = 0;

    if (options.dirs_first) {
        bool left_dir = left->stat_ok && S_ISDIR(left->st.st_mode);
        bool right_dir = right->stat_ok && S_ISDIR(right->st.st_mode);
        if (left_dir != right_dir)
            return left_dir ? -1 : 1;
    }

    if (left->stat_ok != right->stat_ok)
        return left->stat_ok ? -1 : 1;
    if (left->stat_ok && options.sort_key == SORT_TIME) {
        if (left->st.st_mtime > right->st.st_mtime)
            result = -1;
        else if (left->st.st_mtime < right->st.st_mtime)
            result = 1;
    } else if (left->stat_ok && options.sort_key == SORT_SIZE) {
        if (left->st.st_size > right->st.st_size)
            result = -1;
        else if (left->st.st_size < right->st.st_size)
            result = 1;
    }
    if (result == 0 && options.sort_key != SORT_NONE)
        result = strcoll(left->name, right->name);
    return options.reverse ? -result : result;
}

static void free_entries(Entry *entries, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        free(entries[i].name);
        free(entries[i].path);
    }
    free(entries);
}

static uintmax_t stat_size(const struct stat *st)
{
    return st->st_size > 0 ? (uintmax_t)st->st_size : 0;
}

static size_t size_cache_bucket(dev_t device, ino_t inode)
{
    uintmax_t mixed = (uintmax_t)device * UINT64_C(11400714819323198485) ^
                       (uintmax_t)inode;
    return (size_t)(mixed % SIZE_CACHE_BUCKETS);
}

static bool size_cache_find(dev_t device, ino_t inode, uintmax_t *size)
{
    size_t bucket = size_cache_bucket(device, inode);

    for (SizeCache *item = size_cache[bucket]; item != NULL; item = item->next) {
        if (item->device == device && item->inode == inode) {
            *size = item->size;
            return true;
        }
    }
    return false;
}

static SizeCache *size_cache_store(dev_t device, ino_t inode, uintmax_t size)
{
    size_t bucket = size_cache_bucket(device, inode);
    SizeCache *item = xmalloc(sizeof(*item));

    item->device = device;
    item->inode = inode;
    item->size = size;
    item->next = size_cache[bucket];
    size_cache[bucket] = item;
    return item;
}

static uintmax_t add_size(uintmax_t total, uintmax_t amount)
{
    return UINTMAX_MAX - total < amount ? UINTMAX_MAX : total + amount;
}

static uintmax_t directory_total(const char *path, dev_t root_device)
{
    struct stat directory_stat;
    DIR *dir;
    struct dirent *item;
    uintmax_t total;
    SizeCache *cached;

    if (lstat(path, &directory_stat) != 0)
        return 0;
    if (size_cache_find(directory_stat.st_dev, directory_stat.st_ino, &total))
        return total;

    total = stat_size(&directory_stat);
    cached = size_cache_store(directory_stat.st_dev, directory_stat.st_ino,
                              total);
    dir = opendir(path);
    if (dir == NULL)
        return total;

    while ((item = readdir(dir)) != NULL) {
        char *child_path;
        struct stat child_stat;
        uintmax_t child_size;

        if (!strcmp(item->d_name, ".") || !strcmp(item->d_name, ".."))
            continue;
        child_path = join_path(path, item->d_name);
        if (lstat(child_path, &child_stat) == 0) {
            child_size = stat_size(&child_stat);
            if (S_ISDIR(child_stat.st_mode) &&
                (!options.one_filesystem || child_stat.st_dev == root_device))
                child_size = directory_total(child_path, root_device);
            total = add_size(total, child_size);
        }
        free(child_path);
    }
    closedir(dir);
    cached->size = total;
    return total;
}

void free_size_cache(void)
{
    for (size_t bucket = 0; bucket < SIZE_CACHE_BUCKETS; bucket++) {
        SizeCache *item = size_cache[bucket];
        while (item != NULL) {
            SizeCache *next = item->next;
            free(item);
            item = next;
        }
        size_cache[bucket] = NULL;
    }
}

static bool should_descend(const struct stat *st, long depth, dev_t root_device)
{
    return S_ISDIR(st->st_mode) &&
           (options.max_depth < 0 || depth + 1 < options.max_depth) &&
           (!options.one_filesystem || st->st_dev == root_device);
}

uintmax_t displayed_size(const char *path, const struct stat *st, dev_t root_device)
{
    if (options.du && S_ISDIR(st->st_mode))
        return directory_total(path, root_device);
    return stat_size(st);
}

void measure_directory_columns(const char *path, long depth,
                                      dev_t root_device)
{
    DIR *dir = opendir(path);
    struct dirent *item;

    if (dir == NULL)
        return;
    while ((item = readdir(dir)) != NULL) {
        char *child_path;
        struct stat child_stat;
        bool is_dir;
        uintmax_t size;

        if (!strcmp(item->d_name, ".") || !strcmp(item->d_name, ".."))
            continue;
        if (!options.all && item->d_name[0] == '.')
            continue;
        child_path = join_path(path, item->d_name);
        if (lstat(child_path, &child_stat) != 0) {
            free(child_path);
            continue;
        }
        is_dir = S_ISDIR(child_stat.st_mode);
        if (options.dirs_only && !is_dir) {
            free(child_path);
            continue;
        }
        size = displayed_size(child_path, &child_stat, root_device);
        measure_stat_columns(&child_stat, size);

        if (should_descend(&child_stat, depth, root_device))
            measure_directory_columns(child_path, depth + 1, root_device);
        free(child_path);
    }
    closedir(dir);
}

static Entry *read_entries(const char *path, size_t *count, Totals *totals)
{
    DIR *dir = opendir(path);
    Entry *entries = NULL;
    size_t used = 0;
    size_t capacity = 0;
    struct dirent *item;

    *count = 0;
    if (dir == NULL) {
        fprintf(stderr, "ltree: %s: %s\n", path, strerror(errno));
        totals->errors++;
        return NULL;
    }

    errno = 0;
    while ((item = readdir(dir)) != NULL) {
        Entry entry;

        if (!strcmp(item->d_name, ".") || !strcmp(item->d_name, ".."))
            continue;
        if (!options.all && item->d_name[0] == '.')
            continue;

        entry.name = xstrdup(item->d_name);
        entry.path = join_path(path, item->d_name);
        entry.stat_ok = lstat(entry.path, &entry.st) == 0;
        if (!entry.stat_ok) {
            fprintf(stderr, "ltree: %s: %s\n", entry.path, strerror(errno));
            totals->errors++;
        }
        if (options.dirs_only && (!entry.stat_ok || !S_ISDIR(entry.st.st_mode))) {
            free(entry.name);
            free(entry.path);
            continue;
        }
        if (used == capacity) {
            capacity = capacity ? capacity * 2 : 32;
            Entry *grown = realloc(entries, capacity * sizeof(*entries));
            if (grown == NULL) {
                closedir(dir);
                free_entries(entries, used);
                fprintf(stderr, "ltree: out of memory\n");
                exit(2);
            }
            entries = grown;
        }
        entries[used++] = entry;
        errno = 0;
    }
    if (errno != 0) {
        fprintf(stderr, "ltree: %s: %s\n", path, strerror(errno));
        totals->errors++;
    }
    closedir(dir);
    if (used > 1 && (options.sort_key != SORT_NONE || options.dirs_first))
        qsort(entries, used, sizeof(*entries), entry_compare);
    *count = used;
    return entries;
}

void walk_directory(const char *path, long depth, dev_t root_device,
                           bool *last_at_depth, Totals *totals)
{
    size_t count = 0;
    Entry *entries = read_entries(path, &count, totals);

    for (size_t i = 0; i < count; i++) {
        Entry *entry = &entries[i];
        bool is_last = i + 1 == count;
        bool is_dir = entry->stat_ok && S_ISDIR(entry->st.st_mode);
        const char *shown = options.full_path ? entry->path : entry->name;
        uintmax_t size = entry->stat_ok ?
            displayed_size(entry->path, &entry->st, root_device) : 0;

        print_metadata(&entry->st, entry->stat_ok, size);
        print_prefix(last_at_depth, (size_t)depth, is_last);
        print_name(shown, entry->path, &entry->st, entry->stat_ok);
        putchar('\n');

        if (is_dir)
            totals->directories++;
        else
            totals->files++;

        if (entry->stat_ok && should_descend(&entry->st, depth, root_device)) {
            last_at_depth[depth] = is_last;
            walk_directory(entry->path, depth + 1, root_device,
                           last_at_depth, totals);
        }
    }
    free_entries(entries, count);
}
