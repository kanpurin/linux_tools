#define _GNU_SOURCE
#include "../strace_src.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(void) {
    char path[] = "/tmp/strace-src-roundtrip-XXXXXX";
    int fd;
    TraceModel source;
    TraceModel loaded;
    TraceEvent *event;

    fd = mkstemp(path);
    assert(fd >= 0);
    close(fd);
    trace_model_init(&source);
    source.events = calloc(1, sizeof(*source.events));
    assert(source.events != NULL);
    source.count = source.cap = 1;
    event = &source.events[0];
    event->text = strdup("[pid 7] write(1, \"x\", 1) = 1");
    event->pid = 7;
    event->tid = 9;
    event->comm = strdup("worker");
    event->frames = calloc(1, sizeof(*event->frames));
    assert(event->text && event->frames);
    event->frame_count = event->frame_cap = 1;
    event->frames[0].module = strdup("./app");
    event->frames[0].function = strdup("main");
    event->frames[0].offset = strdup("0x12");
    event->frames[0].address = strdup("0x1012");
    event->resolution_state = 1;
    event->source_path = strdup("/src/main.c");
    event->source_line = 12;
    event->source_function = strdup("main");
    event->source_frame = 1;

    assert(trace_model_save(&source, path) == 0);
    assert(trace_model_load(&loaded, path) == 0);
    assert(loaded.count == 1);
    assert(loaded.events[0].pid == 7 && loaded.events[0].tid == 9);
    assert(strcmp(loaded.events[0].comm, "worker") == 0);
    assert(strcmp(loaded.events[0].text, event->text) == 0);
    assert(strcmp(loaded.events[0].frames[0].offset, "0x12") == 0);
    assert(strcmp(loaded.events[0].source_path, "/src/main.c") == 0);
    assert(loaded.events[0].source_line == 12);
    assert(loaded.events[0].source_frame == 1);

    trace_model_free(&loaded);
    trace_model_free(&source);
    unlink(path);
    puts("trace file round-trip tests passed");
    return 0;
}
