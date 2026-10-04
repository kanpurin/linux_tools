#define _GNU_SOURCE
#include "resolve_cache.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define CACHE_SLOTS 4096

typedef struct {
    char *module;
    char *address;
    char *path;
    char *function;
    struct stat binary;
    int line;
    int state;
} ResolveEntry;

static ResolveEntry entries[CACHE_SLOTS];

static size_t cache_slot(const StackFrame *frame) {
    uint64_t hash = UINT64_C(14695981039346656037);
    const unsigned char *p;
    for (p = (const unsigned char *)frame->module; *p; ++p)
        hash = (hash ^ *p) * UINT64_C(1099511628211);
    hash = (hash ^ 0xff) * UINT64_C(1099511628211);
    for (p = (const unsigned char *)frame->address; *p; ++p)
        hash = (hash ^ *p) * UINT64_C(1099511628211);
    return (size_t)(hash % CACHE_SLOTS);
}

static void clear_entry(ResolveEntry *entry) {
    free(entry->module);
    free(entry->address);
    free(entry->path);
    free(entry->function);
    memset(entry, 0, sizeof(*entry));
}

static bool same_binary(const struct stat *a, const struct stat *b) {
    return a->st_dev == b->st_dev && a->st_ino == b->st_ino &&
           a->st_size == b->st_size &&
           a->st_mtim.tv_sec == b->st_mtim.tv_sec &&
           a->st_mtim.tv_nsec == b->st_mtim.tv_nsec &&
           a->st_ctim.tv_sec == b->st_ctim.tv_sec &&
           a->st_ctim.tv_nsec == b->st_ctim.tv_nsec;
}

bool resolve_cache_get(StackFrame *frame) {
    ResolveEntry *entry = &entries[cache_slot(frame)];
    struct stat binary;
    char *path = NULL;
    char *function = NULL;
    if (!entry->module || strcmp(entry->module, frame->module) ||
        strcmp(entry->address, frame->address) ||
        stat(frame->module, &binary) != 0 || !same_binary(&entry->binary, &binary))
        return false;
    if (entry->state == 1) {
        path = strdup(entry->path);
        function = strdup(entry->function);
        if (!path || !function) {
            free(path);
            free(function);
            return false;
        }
    }
    frame->source_path = path;
    frame->source_function = function;
    frame->source_line = entry->line;
    frame->resolution_state = entry->state;
    return true;
}

void resolve_cache_put(const StackFrame *frame) {
    ResolveEntry next = {0};
    ResolveEntry *entry;
    if (stat(frame->module, &next.binary) != 0) return;
    next.module = strdup(frame->module);
    next.address = strdup(frame->address);
    next.state = frame->resolution_state;
    next.line = frame->source_line;
    if (next.state == 1) {
        next.path = strdup(frame->source_path);
        next.function = strdup(frame->source_function);
    }
    if (!next.module || !next.address ||
        (next.state == 1 && (!next.path || !next.function))) {
        clear_entry(&next);
        return;
    }
    entry = &entries[cache_slot(frame)];
    clear_entry(entry);
    *entry = next;
}

void resolve_cache_clear(void) {
    size_t i;
    for (i = 0; i < CACHE_SLOTS; ++i) clear_entry(&entries[i]);
}
