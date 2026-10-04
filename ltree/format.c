#include "ltree.h"

#include <grp.h>
#include <inttypes.h>
#include <limits.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif
#define IDENTITY_MAX 256

typedef struct {
    size_t links;
    size_t owner;
    size_t group;
    size_t size;
} ColumnWidths;

static bool use_color;
static ColumnWidths column_widths = {1, 1, 1, 1};

void initialize_output(void)
{
    const char *term = getenv("TERM");
    use_color = options.color > 0 ||
                (options.color < 0 && isatty(STDOUT_FILENO) &&
                 getenv("NO_COLOR") == NULL && term != NULL && strcmp(term, "dumb"));
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

static void identity_text(const struct stat *st, char owner[IDENTITY_MAX],
                          char group[IDENTITY_MAX])
{
    struct passwd *password = options.numeric_ids ? NULL : getpwuid(st->st_uid);
    struct group *group_entry;

    if (password != NULL)
        snprintf(owner, IDENTITY_MAX, "%s", password->pw_name);
    else
        snprintf(owner, IDENTITY_MAX, "%ju", (uintmax_t)st->st_uid);

    group_entry = options.numeric_ids ? NULL : getgrgid(st->st_gid);
    if (group_entry != NULL)
        snprintf(group, IDENTITY_MAX, "%s", group_entry->gr_name);
    else
        snprintf(group, IDENTITY_MAX, "%ju", (uintmax_t)st->st_gid);
}

static const char *size_text(uintmax_t value, char buf[32])
{
    if (options.human_size)
        return human_size(value, buf);
    snprintf(buf, 32, "%ju", value);
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

void print_metadata(const struct stat *st, bool stat_ok,
                           uintmax_t displayed_size)
{
    char mode[11];
    char size[32];
    char date[32];
    char owner[IDENTITY_MAX];
    char group[IDENTITY_MAX];

    if (!options.long_format)
        return;
    if (!stat_ok) {
        printf("?????????? %*s %-*s %-*s %*s %s ",
               (int)column_widths.links, "?",
               (int)column_widths.owner, "?",
               (int)column_widths.group, "?",
               (int)column_widths.size, "?", "<invalid time>");
        return;
    }

    identity_text(st, owner, group);
    printf("%s %*ju %-*s %-*s %*s ", mode_text(st->st_mode, mode),
           (int)column_widths.links, (uintmax_t)st->st_nlink,
           (int)column_widths.owner, owner,
           (int)column_widths.group, group,
           (int)column_widths.size, size_text(displayed_size, size));
    printf("%s ", modification_time(st->st_mtime, date));
}

void print_name(const char *shown, const char *path,
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

void print_prefix(const bool *last_at_depth, size_t depth, bool is_last)
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

static void update_width(size_t *width, size_t value)
{
    if (value > *width)
        *width = value;
}

void measure_stat_columns(const struct stat *st, uintmax_t displayed_size)
{
    char links[32];
    char owner[IDENTITY_MAX];
    char group[IDENTITY_MAX];
    char size[32];

    snprintf(links, sizeof(links), "%ju", (uintmax_t)st->st_nlink);
    identity_text(st, owner, group);
    size_text(displayed_size, size);
    update_width(&column_widths.links, strlen(links));
    update_width(&column_widths.owner, strlen(owner));
    update_width(&column_widths.group, strlen(group));
    update_width(&column_widths.size, strlen(size));
}
