#ifndef STRACE_SRC_H
#define STRACE_SRC_H

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    char *module;
    char *function;
    char *offset;
    char *address;
    int resolution_state; /* 0: not tried, 1: source found, 2: unavailable */
    char *source_path;
    char *source_function;
    int source_line;
} StackFrame;

typedef struct {
    char *text;
    int pid;
    int tid;
    char *comm;
    StackFrame *frames;
    size_t frame_count;
    size_t frame_cap;
    int resolution_state; /* 0: not tried, 1: source found, 2: unavailable */
    char *source_path;
    char *source_function;
    int source_line;
    size_t source_frame; /* selected frame index + 1; zero means unavailable/unknown */
} TraceEvent;

typedef struct {
    TraceEvent *events;
    size_t count;
    size_t cap;
} TraceModel;

typedef enum {
    FEED_IGNORED,
    FEED_NEW_EVENT,
    FEED_STACK_ADDED
} FeedResult;

void trace_model_init(TraceModel *model);
void trace_model_free(TraceModel *model);
FeedResult trace_model_feed_line(TraceModel *model, const char *line);
bool trace_event_resolve(TraceEvent *event);
bool stack_frame_resolve(StackFrame *frame);
int trace_event_source_frame(TraceEvent *event);
bool trace_event_matches(TraceEvent *event, const char *search,
                         const char *syscalls, const char *source_path,
                         int source_line);
int trace_model_save(TraceModel *model, const char *path);
int trace_model_load(TraceModel *model, const char *path);
bool trace_event_syscall_name(const TraceEvent *event, char *name, size_t size);
const char *trace_event_error(const TraceEvent *event);

#endif
