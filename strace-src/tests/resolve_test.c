#define _GNU_SOURCE
#include "../strace_src.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void) {
    FILE *nm;
    char address[64];
    TraceEvent event;
    StackFrame frame;

    nm = popen("nm -n ./tests/fixture | awk '$3 == \"main\" { print \"0x\" $1; exit }'", "r");
    assert(nm != NULL);
    assert(fgets(address, sizeof(address), nm) != NULL);
    assert(pclose(nm) == 0);
    address[strcspn(address, "\r\n")] = '\0';

    memset(&event, 0, sizeof(event));
    memset(&frame, 0, sizeof(frame));
    frame.module = "./tests/fixture";
    frame.function = "main";
    frame.address = address;
    event.frames = &frame;
    event.frame_count = 1;

    assert(trace_event_resolve(&event));
    assert(event.source_line > 0);
    assert(strstr(event.source_path, "fixture.c") != NULL);
    assert(strcmp(event.source_function, "main") == 0);
    assert(trace_event_source_frame(&event) == 0);
    free(event.source_path);
    free(event.source_function);
    free(frame.source_path);
    free(frame.source_function);
    puts("source resolver tests passed");
    return 0;
}
