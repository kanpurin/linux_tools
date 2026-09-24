#define _GNU_SOURCE
#include "strace_src.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void *xrealloc(void *ptr, size_t size) {
    void *next = realloc(ptr, size);
    if (!next) {
        fprintf(stderr, "strace-src: out of memory\n");
        exit(1);
    }
    return next;
}

static char *xstrndup(const char *text, size_t len) {
    char *copy = malloc(len + 1);
    if (!copy) {
        fprintf(stderr, "strace-src: out of memory\n");
        exit(1);
    }
    memcpy(copy, text, len);
    copy[len] = '\0';
    return copy;
}

static void frame_free(StackFrame *frame) {
    free(frame->module);
    free(frame->function);
    free(frame->offset);
    free(frame->address);
    free(frame->source_path);
    free(frame->source_function);
}

static void event_free(TraceEvent *event) {
    size_t i;
    free(event->text);
    free(event->comm);
    for (i = 0; i < event->frame_count; ++i) {
        frame_free(&event->frames[i]);
    }
    free(event->frames);
    free(event->source_path);
    free(event->source_function);
}

void trace_model_init(TraceModel *model) {
    memset(model, 0, sizeof(*model));
}

void trace_model_free(TraceModel *model) {
    size_t i;
    for (i = 0; i < model->count; ++i) {
        event_free(&model->events[i]);
    }
    free(model->events);
    memset(model, 0, sizeof(*model));
}

/* Accept both interactive strace prefixes ("[pid 123]") and the numeric
 * prefixes used by strace when -f and -o are combined. */
static const char *strip_pid_prefix(const char *line, int *pid) {
    const char *p = line;
    char *end = NULL;
    long value;

    while (isspace((unsigned char)*p)) {
        ++p;
    }
    *pid = 0;
    if (strncmp(p, "[pid", 4) == 0 && isspace((unsigned char)p[4])) {
        p += 4;
        while (isspace((unsigned char)*p)) {
            ++p;
        }
        value = strtol(p, &end, 10);
        if (end != p && *end == ']') {
            *pid = (int)value;
            p = end + 1;
        }
    } else if (isdigit((unsigned char)*p)) {
        value = strtol(p, &end, 10);
        if (end != p && isspace((unsigned char)*end)) {
            *pid = (int)value;
            p = end;
        }
    }
    while (isspace((unsigned char)*p)) {
        ++p;
    }
    return p;
}

static bool parse_frame(const char *body, StackFrame *frame) {
    const char *module_start;
    const char *open;
    const char *close;
    const char *bracket;
    const char *bracket_end;
    const char *function_end;
    const char *offset_start = NULL;
    const char *scan;

    while (isspace((unsigned char)*body)) {
        ++body;
    }
    if (*body != '>') {
        return false;
    }
    ++body;
    while (isspace((unsigned char)*body)) {
        ++body;
    }
    module_start = body;
    open = strchr(module_start, '(');
    bracket = strrchr(module_start, '[');
    bracket_end = bracket ? strchr(bracket, ']') : NULL;
    if (!open || !bracket || !bracket_end || open >= bracket) {
        return false;
    }
    close = bracket;
    while (close > open && isspace((unsigned char)close[-1])) {
        --close;
    }
    if (close <= open || close[-1] != ')') {
        return false;
    }
    --close;
    function_end = close;
    for (scan = close; scan > open + 1; --scan) {
        if (scan[-1] == '+' && scan + 1 < close && scan[0] == '0' && scan[1] == 'x') {
            function_end = scan - 1;
            offset_start = scan;
            break;
        }
    }
    memset(frame, 0, sizeof(*frame));
    frame->module = xstrndup(module_start, (size_t)(open - module_start));
    frame->function = xstrndup(open + 1, (size_t)(function_end - (open + 1)));
    frame->offset = offset_start ? xstrndup(offset_start, (size_t)(close - offset_start))
                                 : xstrndup("", 0);
    frame->address = xstrndup(bracket + 1, (size_t)(bracket_end - (bracket + 1)));
    return true;
}

static void append_event(TraceModel *model, const char *body, int pid) {
    TraceEvent *event;
    size_t needed;

    if (model->count == model->cap) {
        model->cap = model->cap ? model->cap * 2 : 256;
        model->events = xrealloc(model->events, model->cap * sizeof(*model->events));
    }
    event = &model->events[model->count++];
    memset(event, 0, sizeof(*event));
    event->pid = pid;
    event->tid = pid;
    if (pid > 0) {
        needed = (size_t)snprintf(NULL, 0, "[pid %d] %s", pid, body) + 1;
        event->text = malloc(needed);
        if (!event->text) {
            fprintf(stderr, "strace-src: out of memory\n");
            exit(1);
        }
        snprintf(event->text, needed, "[pid %d] %s", pid, body);
    } else {
        event->text = strdup(body);
        if (!event->text) {
            fprintf(stderr, "strace-src: out of memory\n");
            exit(1);
        }
    }
}

static void append_frame(TraceEvent *event, StackFrame *frame) {
    if (event->frame_count == event->frame_cap) {
        event->frame_cap = event->frame_cap ? event->frame_cap * 2 : 8;
        event->frames = xrealloc(event->frames, event->frame_cap * sizeof(*event->frames));
    }
    event->frames[event->frame_count++] = *frame;
    if (event->resolution_state != 0) {
        free(event->source_path);
        free(event->source_function);
        event->source_path = NULL;
        event->source_function = NULL;
        event->source_line = 0;
        event->source_frame = 0;
        event->resolution_state = 0;
    }
}

FeedResult trace_model_feed_line(TraceModel *model, const char *line) {
    int pid;
    const char *body = strip_pid_prefix(line, &pid);
    StackFrame frame;
    size_t len;
    char *clean;

    if (!*body) {
        return FEED_IGNORED;
    }
    if (*body == '>') {
        if (model->count > 0 && parse_frame(body, &frame)) {
            append_frame(&model->events[model->count - 1], &frame);
            return FEED_STACK_ADDED;
        }
        return FEED_IGNORED;
    }
    if (strncmp(body, "+++", 3) == 0 || strncmp(body, "---", 3) == 0 ||
        strncmp(body, "strace:", 7) == 0) {
        return FEED_IGNORED;
    }
    len = strlen(body);
    while (len > 0 && (body[len - 1] == '\n' || body[len - 1] == '\r')) {
        --len;
    }
    clean = xstrndup(body, len);
    append_event(model, clean, pid);
    free(clean);
    return FEED_NEW_EVENT;
}
