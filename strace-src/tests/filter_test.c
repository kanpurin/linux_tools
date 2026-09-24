#include "../strace_src.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    TraceEvent event;
    TraceEvent resumed;
    char syscall[64];
    memset(&event, 0, sizeof(event));
    event.text = "[pid 42] openat(AT_FDCWD, \"/tmp/missing\", O_RDONLY) = -1 ENOENT";
    event.resolution_state = 1;
    event.source_path = "/src/main.c";
    event.source_line = 128;
    event.source_function = "main";

    assert(trace_event_syscall_name(&event, syscall, sizeof(syscall)));
    assert(strcmp(syscall, "openat") == 0);
    assert(strcmp(trace_event_error(&event), "-1 ENOENT") == 0);
    event.text = "write(1, \" = -1 ENOENT\", 12) = 12";
    assert(trace_event_error(&event) == NULL);
    event.text = "[pid 42] openat(AT_FDCWD, \"/tmp/missing\", O_RDONLY) = -1 ENOENT";
    memset(&resumed, 0, sizeof(resumed));
    resumed.text = "[pid 42] <... clone resumed>, child_tidptr=0x1) = 77";
    assert(trace_event_syscall_name(&resumed, syscall, sizeof(syscall)));
    assert(strcmp(syscall, "clone") == 0);
    assert(trace_event_matches(&event, "ENOENT", "openat,read", NULL, 0));
    assert(!trace_event_matches(&event, "EACCES", "openat", NULL, 0));
    assert(!trace_event_matches(&event, "ENOENT", "read,write", NULL, 0));
    assert(trace_event_matches(&event, NULL, NULL, "/src/main.c", 128));
    assert(trace_event_matches(&event, "ENOENT", "openat", "/src/main.c", 128));
    assert(!trace_event_matches(&event, NULL, NULL, "/src/main.c", 129));
    puts("filter tests passed");
    return 0;
}
