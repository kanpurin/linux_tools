#include "../src/gdb.h"

#include <stdio.h>
#include <string.h>

static int wait_stopped(Gdb *g) {
    for (int i = 0; i < 200; ++i) {
        gdb_poll(g, 50);
        if (g->state == GDB_STOPPED || g->state == GDB_EXITED) return 0;
    }
    return -1;
}

int main(int argc, char **argv) {
    if (argc != 3) return 10;
    Gdb g;
    if (gdb_start(&g, argv[1], NULL)) return 11;
    if (gdb_toggle_breakpoint(&g, argv[2], 5)) return 12;
    if (gdb_run(&g) || wait_stopped(&g) || g.state != GDB_STOPPED) return 13;
    if (!g.source_available || !g.fullname[0] || g.line != 5 ||
        strcmp(g.function, "main")) return 14;

    if (gdb_step(&g)) {
        fprintf(stderr, "step failed: %s\n", g.message);
        return 15;
    }
    if (wait_stopped(&g) || g.state != GDB_STOPPED) {
        fprintf(stderr, "step did not stop: state=%d message=%s\n", g.state,
                g.message);
        return 15;
    }
    if (g.source_available || g.fullname[0] || g.line != 0 ||
        strcmp(g.function, "mixed_nodebug_worker") ||
        !g.pc[0] || g.current_instruction < 0) return 16;

    if (gdb_finish(&g)) {
        fprintf(stderr, "finish failed: %s\n", g.message);
        return 17;
    }
    if (wait_stopped(&g) || g.state != GDB_STOPPED) {
        fprintf(stderr, "finish did not stop: state=%d message=%s\n", g.state,
                g.message);
        return 17;
    }
    if (!g.source_available || !g.fullname[0] || strcmp(g.function, "main"))
        return 18;
    gdb_shutdown(&g);
    puts("mixed mode smoke: ok");
    return 0;
}
