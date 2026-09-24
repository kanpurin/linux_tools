#ifndef FD_TRACKER_H
#define FD_TRACKER_H

#include "strace_src.h"

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    int fd;
    int object_id;
    size_t offset;
    size_t length;
} FdRef;

typedef struct {
    FdRef *refs;
    size_t ref_count;
    size_t ref_cap;
    int primary_ref;
    int jump_event;
    int creator_event;
    bool open_after;
} FdEventInfo;

typedef struct {
    int origin_event;
    char *target;
} FdObject;

typedef struct {
    FdEventInfo *events;
    size_t event_count;
    FdObject *objects;
    size_t object_count;
    size_t object_cap;
} FdAnalysis;

FdAnalysis *fd_analysis_build(TraceModel *model);
void fd_analysis_free(FdAnalysis *analysis);
const FdEventInfo *fd_analysis_event(const FdAnalysis *analysis, size_t index);
const FdObject *fd_analysis_object(const FdAnalysis *analysis, int object_id);

#endif
