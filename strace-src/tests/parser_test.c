#include "../strace_src.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    TraceModel model;
    trace_model_init(&model);

    assert(trace_model_feed_line(&model,
        "4120 write(1, \"Hello\\n\", 6) = 6") == FEED_NEW_EVENT);
    assert(trace_model_feed_line(&model,
        "4120  > /usr/lib/x86_64-linux-gnu/libc.so.6(__write+0x17) [0xf8237]") == FEED_STACK_ADDED);
    assert(trace_model_feed_line(&model,
        "4120  > ./app(main+0x17) [0x1160]") == FEED_STACK_ADDED);
    assert(model.count == 1);
    assert(model.events[0].pid == 4120);
    assert(model.events[0].tid == 4120);
    assert(strcmp(model.events[0].text,
                  "[pid 4120] write(1, \"Hello\\n\", 6) = 6") == 0);
    assert(model.events[0].frame_count == 2);
    assert(strcmp(model.events[0].frames[1].module, "./app") == 0);
    assert(strcmp(model.events[0].frames[1].function, "main") == 0);
    assert(strcmp(model.events[0].frames[1].offset, "0x17") == 0);
    assert(strcmp(model.events[0].frames[1].address, "0x1160") == 0);

    assert(trace_model_feed_line(&model,
        "[pid 4124] openat(AT_FDCWD, \"x\", O_RDONLY) = 3") == FEED_NEW_EVENT);
    assert(model.count == 2);
    assert(model.events[1].pid == 4124);
    assert(trace_model_feed_line(&model, "4124 +++ exited with 0 +++") == FEED_IGNORED);
    assert(model.count == 2);

    trace_model_free(&model);
    puts("parser tests passed");
    return 0;
}
