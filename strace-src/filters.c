#include "strace_src.h"

#include <ctype.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

static const char *syscall_start(const char *text) {
    const char *p = text;
    if (strncmp(p, "[pid ", 5) == 0) {
        const char *end = strchr(p, ']');
        if (end) {
            p = end + 1;
            while (isspace((unsigned char)*p)) ++p;
        }
    }
    return p;
}

static size_t syscall_name_length(const char *text) {
    const char *p = syscall_start(text);
    if (strncmp(p, "<... ", 5) == 0) {
        const char *end;
        p += 5;
        end = strstr(p, " resumed>");
        return end ? (size_t)(end - p) : 0;
    }
    const char *open = strchr(p, '(');
    if (!open) return 0;
    return (size_t)(open - p);
}

bool trace_event_syscall_name(const TraceEvent *event, char *name, size_t size) {
    const char *start;
    size_t length;
    if (!event || !name || size == 0) return false;
    start = syscall_start(event->text);
    if (strncmp(start, "<... ", 5) == 0) start += 5;
    length = syscall_name_length(event->text);
    if (length == 0 || length >= size) return false;
    memcpy(name, start, length);
    name[length] = '\0';
    return true;
}

const char *trace_event_error(const TraceEvent *event) {
    const char *match = NULL;
    const char *scan = event ? event->text : NULL;
    if (!scan) return NULL;
    while ((scan = strstr(scan, " = ")) != NULL) {
        match = scan + 3;
        scan += 3;
    }
    if (!match || strncmp(match, "-1 ", 3) != 0 ||
        !isupper((unsigned char)match[3]))
        return NULL;
    return match;
}

static bool syscall_list_contains(const char *list, const char *name, size_t name_len) {
    const char *p = list;
    while (*p) {
        const char *start;
        const char *end;
        while (*p == ',' || isspace((unsigned char)*p)) ++p;
        start = p;
        while (*p && *p != ',') ++p;
        end = p;
        while (end > start && isspace((unsigned char)end[-1])) --end;
        if ((size_t)(end - start) == name_len && strncmp(start, name, name_len) == 0)
            return true;
        if (*p == ',') ++p;
    }
    return false;
}

bool trace_event_matches(TraceEvent *event, const char *search,
                         const char *syscalls, const char *source_path,
                         int source_line) {
    const char *name;
    size_t name_len;
    if (search && *search && !strstr(event->text, search)) return false;
    if (syscalls && *syscalls) {
        name = syscall_start(event->text);
        name_len = syscall_name_length(event->text);
        if (name_len == 0 || !syscall_list_contains(syscalls, name, name_len)) return false;
    }
    if (source_path && *source_path) {
        if (!trace_event_resolve(event) || event->source_line != source_line ||
            strcmp(event->source_path, source_path) != 0)
            return false;
    }
    return true;
}
