#define _GNU_SOURCE
#include "strace_src.h"
#include "resolve_cache.h"

#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <unistd.h>

static double seconds(void) {
    struct timespec now;
    assert(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
    return now.tv_sec + now.tv_nsec / 1e9;
}

static void release_frame(StackFrame *frame) {
    free(frame->source_path);
    free(frame->source_function);
}

static double run(const char *address, bool cold) {
    double start = seconds();
    int i;
    for (i = 0; i < 200; ++i) {
        StackFrame frame = {0};
        if (cold) resolve_cache_clear();
        frame.module = "./tests/fixture";
        frame.address = (char *)address;
        assert(stack_frame_resolve(&frame));
        assert(strstr(frame.source_path, "fixture.c"));
        assert(strcmp(frame.source_function, "main") == 0);
        release_frame(&frame);
    }
    return seconds() - start;
}

int main(void) {
    char address[64];
    FILE *nm = popen("nm -n ./tests/fixture | awk '$3 == \"main\" { print \"0x\" $1; exit }'", "r");
    double cold;
    double warm;
    StackFrame missing = {0};
    StackFrame cached = {0};
    struct stat binary;
    struct timespec times[2];
    assert(nm);
    assert(fgets(address, sizeof(address), nm));
    assert(pclose(nm) == 0);
    address[strcspn(address, "\r\n")] = '\0';
    cold = run(address, true);
    warm = run(address, false);

    missing.module = "./tests/fixture";
    missing.address = "0xffffffffffffffff";
    assert(!stack_frame_resolve(&missing));
    cached.module = missing.module;
    cached.address = missing.address;
    assert(resolve_cache_get(&cached));
    assert(cached.resolution_state == 2);

    /* Rebuilt binaries must invalidate both successful and negative results. */
    assert(stat(missing.module, &binary) == 0);
    times[0] = binary.st_atim;
    times[1] = binary.st_mtim;
    ++times[1].tv_sec;
    assert(utimensat(AT_FDCWD, missing.module, times, 0) == 0);
    assert(!resolve_cache_get(&cached));
    {
        StackFrame changed = {0};
        changed.module = missing.module;
        changed.address = address;
        assert(!resolve_cache_get(&changed));
    }
    times[1] = binary.st_mtim;
    assert(utimensat(AT_FDCWD, missing.module, times, 0) == 0);
    resolve_cache_clear();
    printf("200 independent frames: cold %.6fs, cached %.6fs (%.1fx)\n",
           cold, warm, cold / warm);
    return 0;
}
