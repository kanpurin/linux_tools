#include <dirent.h>
#include <errno.h>
#include <getopt.h>
#include <grp.h>
#include <inttypes.h>
#include <limits.h>
#include <locale.h>
#include <pwd.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define VERSION "2.3.0"
#define SIZE_CACHE_BUCKETS 4099

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
    char *name;
    char *path;
    struct stat st;
    bool stat_ok;
} Entry;

typedef struct {
    unsigned long directories;
    unsigned long files;
    unsigned long errors;
} Totals;

typedef struct SizeCache {
    dev_t device;
    ino_t inode;
    uintmax_t size;
    struct SizeCache *next;
} SizeCache;

static Options options = {
    .report = true,
    .color = -1,
    .max_depth = 1,
    .sort_key = SORT_NAME,
};
static bool use_color;
static SizeCache *size_cache[SIZE_CACHE_BUCKETS];

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

static const char *mode_text(mode_t mode, char text[11])
{
    text[0] = S_ISDIR(mode) ? 'd' : S_ISLNK(mode) ? 'l' :
              S_ISCHR(mode) ? 'c' : S_ISBLK(mode) ? 'b' :
              S_ISFIFO(mode) ? 'p' : S_ISSOCK(mode) ? 's' : '-';
    text[1] = mode & S_IRUSR ? 'r' : '-';
    text[2] = mode & S_IWUSR ? 'w' : '-';
    text[3] = mode & S_ISUID ? (mode & S_IXUSR ? 's' : 'S') :
              (mode & S_IXUSR ? 'x' : '-');
    text[4] = mode & S_IRGRP ? 'r' : '-';
    text[5] = mode & S_IWGRP ? 'w' : '-';
    text[6] = mode & S_ISGID ? (mode & S_IXGRP ? 's' : 'S') :
              (mode & S_IXGRP ? 'x' : '-');
    text[7] = mode & S_IROTH ? 'r' : '-';
    text[8] = mode & S_IWOTH ? 'w' : '-';
    text[9] = mode & S_ISVTX ? (mode & S_IXOTH ? 't' : 'T') :
              (mode & S_IXOTH ? 'x' : '-');
    text[10] = '\0';
    return text;
}

static const char *human_size(uintmax_t bytes, char buf[32])
{
    static const char units[] = "BKMGTPE";
    double value = (double)bytes;
    size_t unit = 0;

    while (value >= 1024.0 && unit < sizeof(units) - 2) {
        value /= 1024.0;
        unit++;
    }
    if (unit == 0)
        snprintf(buf, 32, "%juB", bytes);
    else if (value >= 10.0)
        snprintf(buf, 32, "%.0f%c", value, units[unit]);
    else
        snprintf(buf, 32, "%.1f%c", value, units[unit]);
    return buf;
}

static const char *modification_time(time_t value, char buf[32])
{
    struct tm local;

    if (localtime_r(&value, &local) == NULL ||
        strftime(buf, 32, "%Y-%m-%d %H:%M", &local) == 0)
        snprintf(buf, 32, "<invalid time>");
    return buf;
}

static const char *color_for(const struct stat *st, bool stat_ok)
{
    if (!use_color || !stat_ok)
        return "";
    if (S_ISDIR(st->st_mode))
        return "\033[1;34m";
    if (S_ISLNK(st->st_mode))
        return "\033[1;36m";
    if (S_ISFIFO(st->st_mode))
        return "\033[33m";
    if (S_ISSOCK(st->st_mode))
        return "\033[1;35m";
    if (S_ISBLK(st->st_mode) || S_ISCHR(st->st_mode))
        return "\033[1;33m";
    if (st->st_mode & (S_IXUSR | S_IXGRP | S_IXOTH))
        return "\033[1;32m";
    return "";
}

static char type_suffix(const struct stat *st, bool stat_ok)
{
    if (!stat_ok)
        return '\0';
    if (S_ISDIR(st->st_mode) &&
        (options.classify || options.slash_directories))
        return '/';
    if (!options.classify)
        return '\0';
    if (S_ISLNK(st->st_mode)) return '@';
    if (S_ISFIFO(st->st_mode)) return '|';
    if (S_ISSOCK(st->st_mode)) return '=';
    if (S_ISREG(st->st_mode) && st->st_mode & (S_IXUSR | S_IXGRP | S_IXOTH))
        return '*';
    return '\0';
}

static void print_metadata(const struct stat *st, bool stat_ok,
                           uintmax_t displayed_size)
{
    char mode[11];
    char size[32];
    char date[32];
    char owner_number[32];
    char group_number[32];
    const char *owner;
    const char *group;
    struct passwd *password;
    struct group *group_entry;

    if (!options.long_format)
        return;
    if (!stat_ok) {
        fputs("??????????   ? ?        ?               ? ", stdout);
        return;
    }

    snprintf(owner_number, sizeof(owner_number), "%ju", (uintmax_t)st->st_uid);
    snprintf(group_number, sizeof(group_number), "%ju", (uintmax_t)st->st_gid);
    password = options.numeric_ids ? NULL : getpwuid(st->st_uid);
    group_entry = options.numeric_ids ? NULL : getgrgid(st->st_gid);
    owner = password != NULL ? password->pw_name : owner_number;
    group = group_entry != NULL ? group_entry->gr_name : group_number;

    printf("%s %3ju %-8s %-8s ", mode_text(st->st_mode, mode),
           (uintmax_t)st->st_nlink, owner, group);
    if (options.human_size)
        printf("%7s ", human_size(displayed_size, size));
    else
        printf("%11ju ", displayed_size);
    printf("%s ", modification_time(st->st_mtime, date));
}

static void print_name(const char *shown, const char *path,
                       const struct stat *st, bool stat_ok)
{
    const char *color = color_for(st, stat_ok);
    char suffix = type_suffix(st, stat_ok);

    fputs(color, stdout);
    fputs(shown, stdout);
    if (suffix)
        putchar(suffix);
    if (*color)
        fputs("\033[0m", stdout);

    if (stat_ok && S_ISLNK(st->st_mode)) {
        char target[PATH_MAX + 1];
        ssize_t length = readlink(path, target, PATH_MAX);
        if (length >= 0) {
            target[length] = '\0';
            printf(" -> %s", target);
        }
    }
}

static void print_prefix(const bool *last_at_depth, size_t depth, bool is_last)
{
    if (options.ascii) {
        for (size_t i = 0; i < depth; i++)
            fputs(last_at_depth[i] ? "    " : "|   ", stdout);
        fputs(is_last ? "`-- " : "|-- ", stdout);
    } else {
        for (size_t i = 0; i < depth; i++)
            fputs(last_at_depth[i] ? "    " : "│   ", stdout);
        fputs(is_last ? "└── " : "├── ", stdout);
    }
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

static void free_size_cache(void)
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

static void walk_directory(const char *path, long depth, dev_t root_device,
                           bool *last_at_depth, Totals *totals)
{
    size_t count = 0;
    Entry *entries = read_entries(path, &count, totals);

    for (size_t i = 0; i < count; i++) {
        Entry *entry = &entries[i];
        bool is_last = i + 1 == count;
        bool is_dir = entry->stat_ok && S_ISDIR(entry->st.st_mode);
        const char *shown = options.full_path ? entry->path : entry->name;
        uintmax_t displayed_size = entry->stat_ok ? stat_size(&entry->st) : 0;

        if (options.du && is_dir)
            displayed_size = directory_total(entry->path, root_device);

        print_metadata(&entry->st, entry->stat_ok, displayed_size);
        print_prefix(last_at_depth, (size_t)depth, is_last);
        print_name(shown, entry->path, &entry->st, entry->stat_ok);
        putchar('\n');

        if (is_dir)
            totals->directories++;
        else
            totals->files++;

        if (is_dir && (options.max_depth < 0 || depth + 1 < options.max_depth) &&
            (!options.one_filesystem || entry->st.st_dev == root_device)) {
            last_at_depth[depth] = is_last;
            walk_directory(entry->path, depth + 1, root_device,
                           last_at_depth, totals);
        }
    }
    free_entries(entries, count);
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

int main(int argc, char **argv)
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
    Totals totals = {0};
    bool last_at_depth[PATH_MAX] = {false};

    setlocale(LC_ALL, "");
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
        case 'V': printf("ltree %s\n", VERSION); return 0;
        case OPT_DIRS_ONLY: options.dirs_only = true; break;
        case OPT_DIRSFIRST: options.dirs_first = true; break;
        case OPT_FULL_PATH: options.full_path = true; break;
        case OPT_ONE_FILE_SYSTEM: options.one_filesystem = true; break;
        case OPT_DU: options.du = true; options.long_format = true; break;
        case OPT_COLOR: options.color = parse_color(optarg); break;
        case OPT_ASCII: options.ascii = true; break;
        case OPT_UNICODE: options.ascii = false; break;
        case OPT_NO_SUMMARY: options.report = false; break;
        case OPT_HELP: usage(stdout); return 0;
        case OPT_VERSION: printf("ltree %s\n", VERSION); return 0;
        default: usage(stderr); return 2;
        }
    }

    const char *term = getenv("TERM");
    use_color = options.color > 0 ||
                (options.color < 0 && isatty(STDOUT_FILENO) &&
                 getenv("NO_COLOR") == NULL && term != NULL && strcmp(term, "dumb"));

    int path_count = argc - optind;
    if (path_count == 0)
        path_count = 1;

    for (int i = 0; i < path_count; i++) {
        const char *path = argc == optind ? "." : argv[optind + i];
        struct stat st;
        uintmax_t displayed_size;

        if (i > 0)
            putchar('\n');
        if (lstat(path, &st) != 0) {
            fprintf(stderr, "ltree: %s: %s\n", path, strerror(errno));
            totals.errors++;
            continue;
        }
        displayed_size = stat_size(&st);
        if (options.du && S_ISDIR(st.st_mode))
            displayed_size = directory_total(path, st.st_dev);
        print_metadata(&st, true, displayed_size);
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
