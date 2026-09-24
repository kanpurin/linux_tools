#define _GNU_SOURCE
#include "fd_tracker.h"

#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    int fd;
    int object_id;
    int creator_event;
    bool cloexec;
} Binding;

typedef struct {
    int pid;
    Binding *bindings;
    size_t count;
    size_t cap;
} FdTable;

typedef struct {
    int child;
    int parent;
} ParentMap;

static const char *body_start(const TraceEvent *event) {
    const char *p = event->text;
    if (strncmp(p, "[pid ", 5) == 0) {
        const char *end = strchr(p, ']');
        if (end) p = end + 1;
    }
    while (isspace((unsigned char)*p)) ++p;
    return p;
}

static bool result_value(const TraceEvent *event, int *value, const char **position) {
    const char *equals = strrchr(event->text, '=');
    char *end;
    long parsed;
    if (!equals) return false;
    parsed = strtol(equals + 1, &end, 10);
    if (end == equals + 1 || parsed < INT_MIN || parsed > INT_MAX) return false;
    *value = (int)parsed;
    if (position) {
        const char *p = equals + 1;
        while (isspace((unsigned char)*p)) ++p;
        *position = p;
    }
    return true;
}

static bool argument_fd(const TraceEvent *event, int argument, int *fd,
                        const char **position) {
    const char *p = strchr(body_start(event), '(');
    int current = 0;
    char *end;
    long value;
    if (!p) return false;
    ++p;
    while (current < argument) {
        p = strchr(p, ',');
        if (!p) return false;
        ++p;
        ++current;
    }
    while (isspace((unsigned char)*p)) ++p;
    value = strtol(p, &end, 10);
    if (end == p || value < 0 || value > INT_MAX) return false;
    *fd = (int)value;
    if (position) *position = p;
    return true;
}

static bool syscall_is(const char *name, const char *const *values) {
    size_t i;
    for (i = 0; values[i]; ++i) if (strcmp(name, values[i]) == 0) return true;
    return false;
}

static Binding *find_binding(FdTable *table, int fd) {
    size_t i;
    for (i = 0; i < table->count; ++i) if (table->bindings[i].fd == fd) return &table->bindings[i];
    return NULL;
}

static void remove_binding(FdTable *table, int fd) {
    size_t i;
    for (i = 0; i < table->count; ++i) {
        if (table->bindings[i].fd != fd) continue;
        memmove(&table->bindings[i], &table->bindings[i + 1],
                (table->count - i - 1) * sizeof(*table->bindings));
        --table->count;
        return;
    }
}

static void set_binding(FdTable *table, int fd, int object_id, int creator,
                        bool cloexec) {
    Binding *binding = find_binding(table, fd);
    Binding *next;
    if (!binding) {
        if (table->count == table->cap) {
            size_t cap = table->cap ? table->cap * 2 : 16;
            next = realloc(table->bindings, cap * sizeof(*table->bindings));
            if (!next) return;
            table->bindings = next;
            table->cap = cap;
        }
        binding = &table->bindings[table->count++];
    }
    binding->fd = fd;
    binding->object_id = object_id;
    binding->creator_event = creator;
    binding->cloexec = cloexec;
}

static int find_table(FdTable *tables, size_t count, int pid) {
    size_t i;
    for (i = 0; i < count; ++i) if (tables[i].pid == pid) return (int)i;
    return -1;
}

static int parent_of(ParentMap *parents, size_t count, int child) {
    size_t i;
    for (i = 0; i < count; ++i) if (parents[i].child == child) return parents[i].parent;
    return 0;
}

static FdTable *get_table(FdTable **tables, size_t *count, size_t *cap, int pid,
                          ParentMap *parents, size_t parent_count) {
    int index = find_table(*tables, *count, pid);
    FdTable *next;
    int parent;
    int parent_index;
    if (index >= 0) return &(*tables)[index];
    if (*count == *cap) {
        size_t new_cap = *cap ? *cap * 2 : 8;
        next = realloc(*tables, new_cap * sizeof(**tables));
        if (!next) return NULL;
        *tables = next;
        *cap = new_cap;
    }
    index = (int)(*count)++;
    memset(&(*tables)[index], 0, sizeof((*tables)[index]));
    (*tables)[index].pid = pid;
    parent = parent_of(parents, parent_count, pid);
    parent_index = find_table(*tables, (size_t)index, parent);
    if (parent_index >= 0 && (*tables)[parent_index].count) {
        FdTable *source = &(*tables)[parent_index];
        (*tables)[index].bindings = malloc(source->count * sizeof(*source->bindings));
        if ((*tables)[index].bindings) {
            memcpy((*tables)[index].bindings, source->bindings,
                   source->count * sizeof(*source->bindings));
            (*tables)[index].count = (*tables)[index].cap = source->count;
        }
    }
    return &(*tables)[index];
}

static void add_ref(FdEventInfo *info, const TraceEvent *event, int fd,
                    int object_id, const char *position) {
    FdRef *next;
    if (!position || position < event->text) return;
    if (info->ref_count == info->ref_cap) {
        size_t cap = info->ref_cap ? info->ref_cap * 2 : 4;
        next = realloc(info->refs, cap * sizeof(*info->refs));
        if (!next) return;
        info->refs = next;
        info->ref_cap = cap;
    }
    info->refs[info->ref_count].fd = fd;
    info->refs[info->ref_count].object_id = object_id;
    info->refs[info->ref_count].offset = (size_t)(position - event->text);
    info->refs[info->ref_count].length = (size_t)snprintf(NULL, 0, "%d", fd);
    info->ref_count++;
}

static int add_object(FdAnalysis *analysis, int origin, const char *target) {
    FdObject *next;
    int id;
    if (analysis->object_count == analysis->object_cap) {
        size_t cap = analysis->object_cap ? analysis->object_cap * 2 : 32;
        next = realloc(analysis->objects, cap * sizeof(*analysis->objects));
        if (!next) return -1;
        analysis->objects = next;
        analysis->object_cap = cap;
    }
    id = (int)analysis->object_count++;
    analysis->objects[id].origin_event = origin;
    analysis->objects[id].target = strdup(target ? target : "");
    return id;
}

static void extract_target(const TraceEvent *event, char *target, size_t size) {
    const char *start = strchr(body_start(event), '"');
    const char *end;
    size_t length;
    target[0] = '\0';
    if (!start || !(end = strchr(start + 1, '"'))) return;
    ++start;
    length = (size_t)(end - start);
    if (length >= size) length = size - 1;
    memcpy(target, start, length);
    target[length] = '\0';
}

static bool pipe_fds(const TraceEvent *event, int *left, const char **left_pos,
                     int *right, const char **right_pos) {
    const char *p = strchr(body_start(event), '[');
    char *end;
    long a;
    long b;
    if (!p) return false;
    ++p;
    a = strtol(p, &end, 10);
    if (end == p || a < 0 || a > INT_MAX) return false;
    *left_pos = p;
    p = strchr(end, ',');
    if (!p) return false;
    ++p;
    while (isspace((unsigned char)*p)) ++p;
    b = strtol(p, &end, 10);
    if (end == p || b < 0 || b > INT_MAX) return false;
    *right_pos = p;
    *left = (int)a;
    *right = (int)b;
    return true;
}

static void collect_parents(TraceModel *model, ParentMap **parents_out,
                            size_t *count_out) {
    ParentMap *parents = NULL;
    size_t count = 0;
    size_t cap = 0;
    size_t i;
    for (i = 0; i < model->count; ++i) {
        TraceEvent *event = &model->events[i];
        char name[64];
        int child;
        const char *position;
        if (!trace_event_syscall_name(event, name, sizeof(name)) ||
            (strcmp(name, "fork") && strcmp(name, "vfork") &&
             strcmp(name, "clone") && strcmp(name, "clone3")) ||
            !result_value(event, &child, &position) || child <= 0 ||
            strstr(event->text, "CLONE_THREAD"))
            continue;
        if (count == cap) {
            size_t next_cap = cap ? cap * 2 : 8;
            ParentMap *next = realloc(parents, next_cap * sizeof(*parents));
            if (!next) break;
            parents = next;
            cap = next_cap;
        }
        parents[count].child = child;
        parents[count].parent = event->pid;
        ++count;
    }
    *parents_out = parents;
    *count_out = count;
}

static bool fd_first_syscall(const char *name) {
    static const char *const names[] = {
        "read", "write", "pread64", "pwrite64", "readv", "writev", "close",
        "ioctl", "lseek", "fstat", "newfstatat", "fsync", "fdatasync",
        "ftruncate", "fchmod", "fchown", "flock", "getdents", "getdents64",
        "send", "recv", "sendto", "recvfrom", "sendmsg", "recvmsg",
        "shutdown", "connect", "bind", "listen", "getsockname", "getpeername",
        "setsockopt", "getsockopt", NULL
    };
    return syscall_is(name, names);
}

FdAnalysis *fd_analysis_build(TraceModel *model) {
    static const char *const opens[] = {"open", "openat", "openat2", "creat", NULL};
    static const char *const sockets[] = {"socket", NULL};
    static const char *const accepts[] = {"accept", "accept4", NULL};
    static const char *const pipes[] = {"pipe", "pipe2", NULL};
    static const char *const dups[] = {"dup", "dup2", "dup3", NULL};
    FdAnalysis *analysis = calloc(1, sizeof(*analysis));
    FdTable *tables = NULL;
    size_t table_count = 0;
    size_t table_cap = 0;
    ParentMap *parents = NULL;
    size_t parent_count = 0;
    size_t i;
    if (!analysis) return NULL;
    analysis->event_count = model->count;
    analysis->events = calloc(model->count ? model->count : 1, sizeof(*analysis->events));
    if (!analysis->events) goto fail;
    for (i = 0; i < model->count; ++i) {
        analysis->events[i].primary_ref = -1;
        analysis->events[i].jump_event = -1;
        analysis->events[i].creator_event = -1;
    }
    collect_parents(model, &parents, &parent_count);
    for (i = 0; i < model->count; ++i) {
        TraceEvent *event = &model->events[i];
        FdEventInfo *info = &analysis->events[i];
        FdTable *table = get_table(&tables, &table_count, &table_cap, event->pid,
                                   parents, parent_count);
        char name[64];
        int first_fd;
        int second_fd;
        int result;
        const char *first_pos = NULL;
        const char *second_pos = NULL;
        const char *result_pos = NULL;
        Binding *source;
        if (!table || !trace_event_syscall_name(event, name, sizeof(name))) continue;
        if ((strcmp(name, "fork") == 0 || strcmp(name, "vfork") == 0 ||
             strcmp(name, "clone") == 0 || strcmp(name, "clone3") == 0) &&
            !strstr(event->text, "CLONE_THREAD") &&
            result_value(event, &result, &result_pos) && result > 0) {
            /* Materialize the child's copy at the clone result, before later
             * parent close/dup operations can change the inherited snapshot. */
            (void)get_table(&tables, &table_count, &table_cap, result,
                            parents, parent_count);
            continue;
        }
        if ((strcmp(name, "execve") == 0 || strcmp(name, "execveat") == 0) &&
            result_value(event, &result, &result_pos) && result == 0) {
            size_t b = 0;
            while (b < table->count) {
                if (table->bindings[b].cloexec) remove_binding(table, table->bindings[b].fd);
                else ++b;
            }
            continue;
        }
        if (syscall_is(name, opens) && result_value(event, &result, &result_pos) && result >= 0) {
            char target[512];
            int object;
            extract_target(event, target, sizeof(target));
            object = add_object(analysis, (int)i, target);
            remove_binding(table, result);
            set_binding(table, result, object, (int)i, strstr(event->text, "O_CLOEXEC") != NULL);
            add_ref(info, event, result, object, result_pos);
            info->primary_ref = 0;
            info->creator_event = (int)i;
            info->open_after = true;
            continue;
        }
        if (syscall_is(name, sockets) && result_value(event, &result, &result_pos) && result >= 0) {
            int object = add_object(analysis, (int)i, "socket");
            remove_binding(table, result);
            set_binding(table, result, object, (int)i, strstr(event->text, "SOCK_CLOEXEC") != NULL);
            add_ref(info, event, result, object, result_pos);
            info->primary_ref = 0;
            info->creator_event = (int)i;
            info->open_after = true;
            continue;
        }
        if (syscall_is(name, accepts) && result_value(event, &result, &result_pos) && result >= 0) {
            int object = add_object(analysis, (int)i, "accepted socket");
            argument_fd(event, 0, &first_fd, &first_pos);
            source = find_binding(table, first_fd);
            if (first_pos) add_ref(info, event, first_fd, source ? source->object_id : -1, first_pos);
            remove_binding(table, result);
            set_binding(table, result, object, (int)i, strstr(event->text, "SOCK_CLOEXEC") != NULL);
            add_ref(info, event, result, object, result_pos);
            info->primary_ref = (int)info->ref_count - 1;
            info->creator_event = (int)i;
            info->open_after = true;
            continue;
        }
        if (syscall_is(name, pipes) && result_value(event, &result, &result_pos) && result == 0 &&
            pipe_fds(event, &first_fd, &first_pos, &second_fd, &second_pos)) {
            int object = add_object(analysis, (int)i, "pipe");
            bool cloexec = strstr(event->text, "O_CLOEXEC") != NULL;
            remove_binding(table, first_fd);
            remove_binding(table, second_fd);
            set_binding(table, first_fd, object, (int)i, cloexec);
            set_binding(table, second_fd, object, (int)i, cloexec);
            add_ref(info, event, first_fd, object, first_pos);
            add_ref(info, event, second_fd, object, second_pos);
            info->primary_ref = 0;
            info->creator_event = (int)i;
            info->open_after = true;
            continue;
        }
        if (syscall_is(name, dups) && argument_fd(event, 0, &first_fd, &first_pos) &&
            result_value(event, &result, &result_pos) && result >= 0) {
            source = find_binding(table, first_fd);
            add_ref(info, event, first_fd, source ? source->object_id : -1, first_pos);
            if ((strcmp(name, "dup2") == 0 || strcmp(name, "dup3") == 0) &&
                argument_fd(event, 1, &second_fd, &second_pos))
                add_ref(info, event, second_fd, source ? source->object_id : -1, second_pos);
            if (source) {
                int object = source->object_id;
                int previous = source->creator_event;
                remove_binding(table, result);
                set_binding(table, result, object, (int)i,
                            strcmp(name, "dup3") == 0 && strstr(event->text, "O_CLOEXEC"));
                add_ref(info, event, result, object, result_pos);
                info->primary_ref = (int)info->ref_count - 1;
                info->jump_event = previous;
                info->creator_event = (int)i;
                info->open_after = true;
            }
            continue;
        }
        if (strcmp(name, "fcntl") == 0 && argument_fd(event, 0, &first_fd, &first_pos)) {
            source = find_binding(table, first_fd);
            add_ref(info, event, first_fd, source ? source->object_id : -1, first_pos);
            info->primary_ref = 0;
            if ((strstr(event->text, "F_DUPFD") || strstr(event->text, "F_DUPFD_CLOEXEC")) &&
                result_value(event, &result, &result_pos) && result >= 0 && source) {
                int object = source->object_id;
                int previous = source->creator_event;
                remove_binding(table, result);
                set_binding(table, result, object, (int)i,
                            strstr(event->text, "F_DUPFD_CLOEXEC") != NULL);
                add_ref(info, event, result, object, result_pos);
                info->primary_ref = (int)info->ref_count - 1;
                info->jump_event = previous;
                info->creator_event = (int)i;
                info->open_after = true;
            } else if (strstr(event->text, "F_SETFD") && source &&
                       result_value(event, &result, &result_pos) && result == 0) {
                source->cloexec = strstr(event->text, "FD_CLOEXEC") != NULL;
                info->jump_event = source->creator_event;
                info->creator_event = source->creator_event;
                info->open_after = true;
            }
            continue;
        }
        if (fd_first_syscall(name) && argument_fd(event, 0, &first_fd, &first_pos)) {
            source = find_binding(table, first_fd);
            add_ref(info, event, first_fd, source ? source->object_id : -1, first_pos);
            info->primary_ref = 0;
            if (source) {
                info->jump_event = source->creator_event;
                info->creator_event = source->creator_event;
                info->open_after = true;
                if (strcmp(name, "close") == 0 &&
                    result_value(event, &result, &result_pos) && result == 0) {
                    info->open_after = false;
                    remove_binding(table, first_fd);
                }
            }
        }
    }
    for (i = 0; i < table_count; ++i) free(tables[i].bindings);
    free(tables);
    free(parents);
    return analysis;
fail:
    free(parents);
    fd_analysis_free(analysis);
    return NULL;
}

void fd_analysis_free(FdAnalysis *analysis) {
    size_t i;
    if (!analysis) return;
    for (i = 0; i < analysis->event_count; ++i) free(analysis->events[i].refs);
    for (i = 0; i < analysis->object_count; ++i) free(analysis->objects[i].target);
    free(analysis->events);
    free(analysis->objects);
    free(analysis);
}

const FdEventInfo *fd_analysis_event(const FdAnalysis *analysis, size_t index) {
    if (!analysis || index >= analysis->event_count) return NULL;
    return &analysis->events[index];
}

const FdObject *fd_analysis_object(const FdAnalysis *analysis, int object_id) {
    if (!analysis || object_id < 0 || (size_t)object_id >= analysis->object_count) return NULL;
    return &analysis->objects[object_id];
}
