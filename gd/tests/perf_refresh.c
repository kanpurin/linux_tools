#include "../src/gdb.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double milliseconds(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000.0 + t.tv_nsec / 1000000.0;
}

static int compare(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

int main(int argc, char **argv) {
    if (argc != 2 && argc != 3)
        return 2;
    static Gdb g;
    if (gdb_start(&g, argv[1], NULL) || gdb_set_function_breakpoint(&g, "main") || gdb_run(&g))
        return 3;
    for (int i = 0; i < 200 && g.state != GDB_STOPPED; i++)
        if (gdb_poll(&g, 50) < 0)
            return 4;
    if (g.state != GDB_STOPPED)
        return 5;
    g.defer_disassembly = argc == 3 && !strcmp(argv[2], "source");
    enum { SAMPLES = 100 };
    double times[SAMPLES];
    int token = g.token;
    for (int i = 0; i < SAMPLES; i++) {
        double start = milliseconds();
        if (gdb_refresh(&g) || !g.register_count || (!g.defer_disassembly && !g.instruction_count))
            return 6;
        times[i] = milliseconds() - start;
    }
    qsort(times, SAMPLES, sizeof(times[0]), compare);
    printf("refresh (%s): median=%.3fms p95=%.3fms commands=%.1f/refresh (%d samples)\n",
           g.defer_disassembly ? "Source UI" : "full API", times[SAMPLES / 2],
           times[SAMPLES * 95 / 100], (double)(g.token - token) / SAMPLES, SAMPLES);
    gdb_shutdown(&g);
    return 0;
}
