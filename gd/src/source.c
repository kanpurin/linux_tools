#include "source.h"
#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

static bool same_metadata(const struct stat *a, const struct stat *b) {
    return a->st_dev == b->st_dev && a->st_ino == b->st_ino && a->st_mode == b->st_mode &&
           a->st_size == b->st_size && a->st_mtim.tv_sec == b->st_mtim.tv_sec &&
           a->st_mtim.tv_nsec == b->st_mtim.tv_nsec && a->st_ctim.tv_sec == b->st_ctim.tv_sec &&
           a->st_ctim.tv_nsec == b->st_ctim.tv_nsec;
}

void source_free(Source *s) {
    for (int i = 0; i < s->count; i++)
        free(s->lines[i]);
    free(s->lines);
    memset(s, 0, sizeof(*s));
}
int source_load(Source *s, const char *path) {
    struct stat st;
    if (!path || !*path)
        return -1;
    if (stat(path, &st))
        return -1;
    if (!strcmp(s->path, path) && s->mtime == st.st_mtime && same_metadata(&s->metadata, &st))
        return 0;
    FILE *fp = fopen(path, "r");
    if (!fp)
        return -1;
    source_free(s);
    snprintf(s->path, sizeof(s->path), "%s", path);
    s->mtime = st.st_mtime;
    s->metadata = st;
    char *line = NULL;
    size_t cap = 0;
    ssize_t n;
    size_t line_capacity = 0;
    while ((n = getline(&line, &cap, fp)) >= 0) {
        if (n && line[n - 1] == '\n')
            line[--n] = '\0';
        if ((size_t)s->count == line_capacity) {
            size_t next_capacity = line_capacity ? line_capacity * 2 : 128;
            char **v = realloc(s->lines, next_capacity * sizeof(*v));
            if (!v) {
                free(line);
                fclose(fp);
                source_free(s);
                return -1;
            }
            s->lines = v;
            line_capacity = next_capacity;
        }
        char *copy = strdup(line);
        if (!copy) {
            free(line);
            fclose(fp);
            source_free(s);
            return -1;
        }
        s->lines[s->count++] = copy;
    }
    free(line);
    fclose(fp);
    return 0;
}
const char *path_name(const char *path) {
    const char *p = strrchr(path, '/');
    return p ? p + 1 : path;
}
static void path_directory(const char *path, char *out, size_t size) {
    snprintf(out, size, "%s", path ? path : "");
    char *p = strrchr(out, '/');
    if (p) {
        if (p == out)
            p[1] = '\0';
        else
            *p = '\0';
    } else
        snprintf(out, size, ".");
}
void project_root_init(Gdb *g, char *out, size_t size) {
    char exe[GD_PATH_MAX] = "", src[GD_PATH_MAX] = "", exe_dir[GD_PATH_MAX], src_dir[GD_PATH_MAX];
    if (!realpath(g->executable, exe))
        snprintf(exe, sizeof(exe), "%s", g->executable);
    path_directory(exe, exe_dir, sizeof(exe_dir));
    if (!g->fullname[0] || !realpath(g->fullname, src)) {
        snprintf(out, size, "%s", exe_dir);
        return;
    }
    path_directory(src, src_dir, sizeof(src_dir));
    size_t last = 0, i = 0;
    while (exe_dir[i] && src_dir[i] && exe_dir[i] == src_dir[i]) {
        if (exe_dir[i] == '/')
            last = i;
        i++;
    }
    if (!exe_dir[i] && !src_dir[i])
        snprintf(out, size, "%s", src_dir);
    else if (last > 0) {
        size_t n = last;
        if (n >= size)
            n = size - 1;
        memcpy(out, src_dir, n);
        out[n] = '\0';
    } else
        snprintf(out, size, "%s", exe_dir);
}
bool project_path(const char *path, const char *root) {
    if (!path || !*path)
        return false;
    if (path[0] != '/')
        return true;
    size_t n = strlen(root);
    return n > 0 && !strncmp(path, root, n) && (path[n] == '/' || path[n] == '\0');
}
static bool source_extension(const char *path) {
    const char *dot = strrchr(path, '.');
    if (!dot)
        return false;
    const char *extensions[] = {".c", ".h", ".cc", ".hh", ".cpp", ".hpp", ".cxx", ".hxx", NULL};
    for (int i = 0; extensions[i]; i++)
        if (!strcasecmp(dot, extensions[i]))
            return true;
    return false;
}
bool same_source_path(const char *a, const char *b) {
    if (!a || !b || !*a || !*b)
        return false;
    if (!strcmp(a, b))
        return true;
    char ra[GD_PATH_MAX], rb[GD_PATH_MAX];
    return realpath(a, ra) && realpath(b, rb) && !strcmp(ra, rb);
}
static bool debug_source_path(GdbSourceFile *files, int count, const char *path) {
    for (int i = 0; i < count; i++)
        if (!strcmp(path, files[i].fullname[0] ? files[i].fullname : files[i].file))
            return true;
    return false;
}
static int source_candidate_compare(const void *left, const void *right) {
    const SourceFileCandidate *a = left, *b = right;
    return strcmp(a->display, b->display);
}

/* Some filesystems give multiple directory edits the same timestamp. Verify
   membership as well; this does not stat/realpath every source file again. */
static bool directory_signature(const char *path, unsigned long long *signature, size_t *count) {
    DIR *directory = opendir(path);
    if (!directory)
        return false;
    *signature = 0;
    *count = 0;
    struct dirent *entry;
    while ((entry = readdir(directory))) {
        unsigned long long hash = 14695981039346656037ULL;
        for (const unsigned char *p = (const unsigned char *)entry->d_name; *p; p++)
            hash = (hash ^ *p) * 1099511628211ULL;
        hash = (hash ^ entry->d_ino) * 1099511628211ULL;
        hash = (hash ^ entry->d_type) * 1099511628211ULL;
        *signature ^= hash;
        (*count)++;
    }
    closedir(directory);
    return true;
}
static void scan_source_tree(const char *root, const char *dir, GdbSourceFile *debug_files,
                             int debug_count, FileDialog *d, int depth) {
    if (depth > 16 || d->all_count >= UI_MAX_SOURCE_FILES)
        return;
    DIR *dp = opendir(dir);
    if (!dp) {
        d->catalogue_valid = false;
        return;
    }
    if (d->directory_count < UI_MAX_SOURCE_DIRS) {
        SourceDirectory *entry = &d->directories[d->directory_count++];
        snprintf(entry->path, sizeof(entry->path), "%s", dir);
        if (stat(dir, &entry->metadata))
            d->catalogue_valid = false;
        if (!directory_signature(dir, &entry->signature, &entry->entries))
            d->catalogue_valid = false;
    } else
        d->catalogue_valid = false;
    struct dirent *entry;
    while ((entry = readdir(dp)) && d->all_count < UI_MAX_SOURCE_FILES) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..") ||
            !strcmp(entry->d_name, ".git"))
            continue;
        char path[GD_PATH_MAX];
        if (snprintf(path, sizeof(path), "%s/%s", dir, entry->d_name) >= (int)sizeof(path))
            continue;
        struct stat st;
        if (lstat(path, &st) || S_ISLNK(st.st_mode))
            continue;
        if (S_ISDIR(st.st_mode)) {
            scan_source_tree(root, path, debug_files, debug_count, d, depth + 1);
            continue;
        }
        if (!S_ISREG(st.st_mode) || !source_extension(path))
            continue;
        char canonical[GD_PATH_MAX];
        if (!realpath(path, canonical))
            snprintf(canonical, sizeof(canonical), "%s", path);
        bool duplicate = false;
        for (int i = 0; i < d->all_count; i++)
            if (!strcmp(d->all[i].canonical, canonical)) {
                duplicate = true;
                break;
            }
        if (duplicate)
            continue;
        SourceFileCandidate *c = &d->all[d->all_count++];
        memset(c, 0, sizeof(*c));
        snprintf(c->path, sizeof(c->path), "%s", path);
        snprintf(c->canonical, sizeof(c->canonical), "%s", canonical);
        size_t root_len = strlen(root);
        snprintf(c->display, sizeof(c->display), "%s",
                 !strncmp(path, root, root_len) && path[root_len] == '/' ? path + root_len + 1
                                                                         : path);
        c->debug_lines = debug_source_path(debug_files, debug_count, canonical);
    }
    closedir(dp);
}
void filter_file_dialog(FileDialog *d) {
    d->count = 0;
    d->selected = 0;
    d->top = 0;
    for (int i = 0; i < d->all_count; i++)
        if (!d->input[0] || strstr(d->all[i].display, d->input) ||
            strstr(path_name(d->all[i].path), d->input))
            d->matches[d->count++] = i;
}
void load_file_dialog(Gdb *g, const char *project_root, FileDialog *d) {
    bool reusable = d->catalogue_valid && d->owner == g->pid &&
                    d->symbol_revision == g->symbol_revision && !strcmp(d->root, project_root);
    for (int i = 0; reusable && i < d->directory_count; i++) {
        struct stat current;
        unsigned long long signature;
        size_t entries;
        if (stat(d->directories[i].path, &current) ||
            !same_metadata(&current, &d->directories[i].metadata) ||
            !directory_signature(d->directories[i].path, &signature, &entries) ||
            signature != d->directories[i].signature || entries != d->directories[i].entries)
            reusable = false;
    }
    if (reusable) {
        d->input[0] = '\0';
        filter_file_dialog(d);
        return;
    }
    memset(d, 0, sizeof(*d));
    d->owner = g->pid;
    snprintf(d->root, sizeof(d->root), "%s", project_root);
    d->catalogue_valid = true;
    GdbSourceFile *debug_files = malloc(UI_MAX_SOURCE_FILES * sizeof(*debug_files));
    int debug_count = 0;
    if (!debug_files || gdb_list_source_files(g, debug_files, UI_MAX_SOURCE_FILES, &debug_count)) {
        debug_count = 0;
        d->catalogue_valid = false;
    }
    for (int i = 0; i < debug_count; i++) {
        char canonical[GD_PATH_MAX];
        const char *path =
            debug_files[i].fullname[0] ? debug_files[i].fullname : debug_files[i].file;
        if (realpath(path, canonical))
            snprintf(debug_files[i].fullname, sizeof(debug_files[i].fullname), "%s", canonical);
    }
    scan_source_tree(project_root, project_root, debug_files, debug_count, d, 0);
    free(debug_files);
    d->symbol_revision = g->symbol_revision;
    if (d->all_count == UI_MAX_SOURCE_FILES)
        d->catalogue_valid = false;
    qsort(d->all, (size_t)d->all_count, sizeof(d->all[0]), source_candidate_compare);
    filter_file_dialog(d);
}
void history_update(SourceHistory *h, Source *s, int cursor, int column, int top, int hscroll) {
    if (h->index < 0 || h->index >= h->count || !s->path[0])
        return;
    SourceLocation *loc = &h->items[h->index];
    if (loc->code_view != CODE_SOURCE || strcmp(loc->path, s->path))
        return;
    loc->cursor = cursor;
    loc->column = column;
    loc->top = top;
    loc->hscroll = hscroll;
    loc->debug_lines = s->debug_lines;
}
void history_update_assembly(SourceHistory *h, int selected, int top) {
    if (h->index < 0 || h->index >= h->count)
        return;
    SourceLocation *loc = &h->items[h->index];
    if (loc->code_view != CODE_ASSEMBLY)
        return;
    loc->asm_selected = selected;
    loc->asm_top = top;
}
void history_push(SourceHistory *h, const char *path, int cursor, int column, int top, int hscroll,
                  bool debug_lines) {
    if (!path || !*path)
        return;
    if (h->index >= 0 && h->index < h->count) {
        SourceLocation *current = &h->items[h->index];
        if (current->code_view == CODE_SOURCE && !strcmp(current->path, path) &&
            current->cursor == cursor) {
            current->column = column;
            current->top = top;
            current->hscroll = hscroll;
            current->debug_lines = debug_lines;
            return;
        }
    }
    h->count = h->index + 1;
    if (h->count >= UI_HISTORY_MAX) {
        memmove(&h->items[0], &h->items[1], (UI_HISTORY_MAX - 1) * sizeof(h->items[0]));
        h->count = UI_HISTORY_MAX - 1;
        h->index--;
    }
    SourceLocation *loc = &h->items[h->count++];
    memset(loc, 0, sizeof(*loc));
    loc->code_view = CODE_SOURCE;
    snprintf(loc->path, sizeof(loc->path), "%s", path);
    loc->cursor = cursor;
    loc->column = column;
    loc->top = top;
    loc->hscroll = hscroll;
    loc->debug_lines = debug_lines;
    h->index = h->count - 1;
}
void history_push_assembly(SourceHistory *h, const char *address, const char *function,
                           int selected, int top) {
    if (!address || !*address)
        return;
    if (h->index >= 0 && h->index < h->count) {
        SourceLocation *current = &h->items[h->index];
        if (current->code_view == CODE_ASSEMBLY && !strcmp(current->address, address)) {
            current->asm_selected = selected;
            current->asm_top = top;
            return;
        }
    }
    h->count = h->index + 1;
    if (h->count >= UI_HISTORY_MAX) {
        memmove(&h->items[0], &h->items[1], (UI_HISTORY_MAX - 1) * sizeof(h->items[0]));
        h->count = UI_HISTORY_MAX - 1;
        h->index--;
    }
    SourceLocation *loc = &h->items[h->count++];
    memset(loc, 0, sizeof(*loc));
    loc->code_view = CODE_ASSEMBLY;
    snprintf(loc->address, sizeof(loc->address), "%s", address);
    snprintf(loc->function, sizeof(loc->function), "%s", function ? function : "");
    loc->asm_selected = selected;
    loc->asm_top = top;
    h->index = h->count - 1;
}
bool source_navigate(Source *s, SourceHistory *h, const char *path, int line, int *cursor,
                     int *column, int *top, int *hscroll, bool debug_lines, bool record) {
    if (!path || !*path || line < 1)
        return false;
    history_update(h, s, *cursor, *column, *top, *hscroll);
    if (source_load(s, path))
        return false;
    s->debug_lines = debug_lines;
    *cursor = line - 1;
    if (*cursor < 0)
        *cursor = 0;
    if (*cursor >= s->count)
        *cursor = s->count ? s->count - 1 : 0;
    *column = 0;
    *hscroll = 0;
    *top = *cursor;
    if (record)
        history_push(h, s->path, *cursor, *column, *top, *hscroll, debug_lines);
    return true;
}
bool history_move(SourceHistory *h, Source *s, Gdb *g, int direction, CodeView *code_view,
                  int *cursor, int *column, int *top, int *hscroll, int *asm_selected,
                  int *asm_top) {
    int target = h->index + direction;
    if (target < 0 || target >= h->count)
        return false;
    SourceLocation *loc = &h->items[target];
    if (loc->code_view == CODE_SOURCE) {
        if (source_load(s, loc->path))
            return false;
        s->debug_lines = loc->debug_lines;
        *cursor = loc->cursor;
        *column = loc->column;
        *top = loc->top;
        *hscroll = loc->hscroll;
        *code_view = CODE_SOURCE;
    } else {
        if (gdb_disassemble_at(g, loc->address))
            return false;
        *asm_selected = loc->asm_selected;
        *asm_top = loc->asm_top;
        *code_view = CODE_ASSEMBLY;
    }
    h->index = target;
    return true;
}
int source_search(Source *s, int cursor, const char *needle, int direction) {
    if (!s->count || !needle || !*needle)
        return -1;
    for (int offset = 1; offset <= s->count; offset++) {
        int line = (cursor + direction * offset) % s->count;
        if (line < 0)
            line += s->count;
        if (strstr(s->lines[line], needle))
            return line;
    }
    return -1;
}
int text_column(const char *line, const char *needle, int direction) {
    if (direction > 0) {
        const char *p = strstr(line, needle);
        return p ? (int)(p - line) : 0;
    }
    const char *p = line, *last = NULL;
    while ((p = strstr(p, needle))) {
        last = p;
        p++;
    }
    return last ? (int)(last - line) : 0;
}
bool text_search_position(Source *s, int *line, int *column, const char *needle, int direction,
                          bool whole_word) {
    if (!s->count || !needle || !*needle)
        return false;
    int nlen = (int)strlen(needle);
    for (int off = 0; off <= s->count; off++) {
        int ln = (*line + direction * off) % s->count;
        if (ln < 0)
            ln += s->count;
        const char *t = s->lines[ln];
        int len = (int)strlen(t), found = -1;
        if (direction > 0) {
            int start = off == 0 ? *column + 1 : 0;
            if (start < 0)
                start = 0;
            for (int i = start; i + nlen <= len; i++)
                if (!strncmp(t + i, needle, (size_t)nlen) &&
                    (!whole_word || ((i == 0 || !word_char(t[i - 1])) &&
                                     (i + nlen == len || !word_char(t[i + nlen]))))) {
                    found = i;
                    break;
                }
        } else {
            int limit = off == 0 ? *column - 1 : len - nlen;
            if (limit > len - nlen)
                limit = len - nlen;
            for (int i = limit; i >= 0; i--)
                if (!strncmp(t + i, needle, (size_t)nlen) &&
                    (!whole_word || ((i == 0 || !word_char(t[i - 1])) &&
                                     (i + nlen == len || !word_char(t[i + nlen]))))) {
                    found = i;
                    break;
                }
        }
        if (found >= 0) {
            *line = ln;
            *column = found;
            return true;
        }
    }
    return false;
}
bool word_char(int c) {
    return isalnum((unsigned char)c) || c == '_';
}
void clamp_column(Source *s, int line, int *column) {
    if (!s->count) {
        *column = 0;
        return;
    }
    int len = (int)strlen(s->lines[line]);
    if (*column < 0)
        *column = 0;
    if (*column >= len)
        *column = len ? len - 1 : 0;
}
bool word_under_cursor(Source *s, int line, int column, char *out, size_t size, int *start_out) {
    if (!s->count || line < 0 || line >= s->count)
        return false;
    const char *t = s->lines[line];
    int len = (int)strlen(t);
    if (!len)
        return false;
    if (column >= len)
        column = len - 1;
    if (!word_char(t[column])) {
        while (column < len && !word_char(t[column]))
            column++;
        if (column >= len)
            return false;
    }
    int start = column, end = column + 1;
    while (start > 0 && word_char(t[start - 1]))
        start--;
    while (end < len && word_char(t[end]))
        end++;
    size_t n = (size_t)(end - start);
    if (n >= size)
        n = size - 1;
    memcpy(out, t + start, n);
    out[n] = '\0';
    if (start_out)
        *start_out = start;
    return true;
}
static int word_occurrence(const char *line, const char *word, int from, int direction) {
    int len = (int)strlen(line), wlen = (int)strlen(word);
    if (!wlen)
        return -1;
    if (direction > 0) {
        for (int i = from < 0 ? 0 : from; i + wlen <= len; i++)
            if ((i == 0 || !word_char(line[i - 1])) && !strncmp(line + i, word, (size_t)wlen) &&
                (i + wlen == len || !word_char(line[i + wlen])))
                return i;
    } else {
        if (from > len - wlen)
            from = len - wlen;
        for (int i = from; i >= 0; i--)
            if ((i == 0 || !word_char(line[i - 1])) && !strncmp(line + i, word, (size_t)wlen) &&
                (i + wlen == len || !word_char(line[i + wlen])))
                return i;
    }
    return -1;
}
bool search_word(Source *s, int *line, int *column, const char *word, int direction) {
    if (!s->count)
        return false;
    for (int off = 0; off < s->count; off++) {
        int ln = (*line + direction * off) % s->count;
        if (ln < 0)
            ln += s->count;
        int from;
        if (off == 0)
            from = *column + direction;
        else
            from = direction > 0 ? 0 : (int)strlen(s->lines[ln]);
        int col = word_occurrence(s->lines[ln], word, from, direction);
        if (col >= 0) {
            *line = ln;
            *column = col;
            return true;
        }
    }
    return false;
}
void word_motion(Source *s, int *line, int *column, int direction, bool to_end) {
    if (!s->count)
        return;
    int ln = *line, col = *column;
    for (int pass = 0; pass < s->count + 1; pass++) {
        const char *t = s->lines[ln];
        int len = (int)strlen(t);
        if (direction > 0) {
            if (to_end) {
                if (col < len && word_char(t[col]))
                    while (col + 1 < len && word_char(t[col + 1]))
                        col++;
                else {
                    while (col < len && !word_char(t[col]))
                        col++;
                    if (col < len)
                        while (col + 1 < len && word_char(t[col + 1]))
                            col++;
                }
            } else {
                if (col < len && word_char(t[col]))
                    while (col < len && word_char(t[col]))
                        col++;
                while (col < len && !word_char(t[col]))
                    col++;
            }
            if (col < len) {
                *line = ln;
                *column = col;
                return;
            }
            ln++;
            col = 0;
            if (ln >= s->count)
                ln = 0;
        } else {
            col--;
            while (col >= 0 && !word_char(t[col]))
                col--;
            while (col > 0 && word_char(t[col - 1]))
                col--;
            if (col >= 0) {
                *line = ln;
                *column = col;
                return;
            }
            ln--;
            if (ln < 0)
                ln = s->count - 1;
            col = (int)strlen(s->lines[ln]);
        }
    }
}
bool goto_declaration(Source *s, int *line, int *column, bool global) {
    char word[256];
    if (!word_under_cursor(s, *line, *column, word, sizeof(word), NULL))
        return false;
    int fallback_line = -1, fallback_col = 0;
    int start = global ? 0 : *line - 1, end = global ? s->count : *line, step = global ? 1 : -1;
    for (int ln = start; global ? ln < end : ln >= 0; ln += step) {
        int col = word_occurrence(s->lines[ln], word, global ? 0 : (int)strlen(s->lines[ln]),
                                  global ? 1 : -1);
        if (col < 0)
            continue;
        if (fallback_line < 0) {
            fallback_line = ln;
            fallback_col = col;
        }
        const char *t = s->lines[ln];
        bool declaration = false;
        const char *types[] = {"int",   "char",   "long",     "short",  "float",    "double",
                               "void",  "struct", "enum",     "union",  "size_t",   "bool",
                               "const", "static", "unsigned", "signed", "volatile", NULL};
        for (int i = 0; types[i]; i++)
            if (strstr(t, types[i]) && strstr(t, types[i]) < t + col) {
                declaration = true;
                break;
            }
        if (declaration) {
            *line = ln;
            *column = col;
            return true;
        }
    }
    if (fallback_line >= 0) {
        *line = fallback_line;
        *column = fallback_col;
        return true;
    }
    return false;
}
bool match_bracket(Source *s, int *line, int *column) {
    if (!s->count)
        return false;
    int ln = *line, col = *column;
    const char *opens = "([{", *closes = ")]}", *t = s->lines[ln];
    int len = (int)strlen(t), kind = -1, direction = 1;
    for (int i = col; i < len; i++) {
        const char *p = strchr(opens, t[i]);
        const char *q = strchr(closes, t[i]);
        if (p) {
            kind = (int)(p - opens);
            col = i;
            direction = 1;
            break;
        }
        if (q) {
            kind = (int)(q - closes);
            col = i;
            direction = -1;
            break;
        }
    }
    if (kind < 0)
        return false;
    char open = opens[kind], close = closes[kind];
    int depth = 0;
    for (int guard = 0; guard < 100000; guard++) {
        col += direction;
        while (ln >= 0 && ln < s->count && (col < 0 || col >= (int)strlen(s->lines[ln]))) {
            if (direction > 0) {
                ln++;
                col = 0;
            } else {
                ln--;
                if (ln >= 0)
                    col = (int)strlen(s->lines[ln]) - 1;
            }
        }
        if (ln < 0 || ln >= s->count)
            break;
        char c = s->lines[ln][col];
        if (c == (direction > 0 ? open : close))
            depth++;
        else if (c == (direction > 0 ? close : open)) {
            if (depth == 0) {
                *line = ln;
                *column = col;
                return true;
            }
            depth--;
        }
    }
    return false;
}
void paragraph_motion(Source *s, int *line, int direction) {
    if (!s->count)
        return;
    int ln = *line + direction;
    while (ln > 0 && ln < s->count && s->lines[ln][0] == '\0')
        ln += direction;
    while (ln > 0 && ln < s->count && s->lines[ln][0] != '\0')
        ln += direction;
    if (ln < 0)
        ln = 0;
    if (ln >= s->count)
        ln = s->count - 1;
    *line = ln;
}
