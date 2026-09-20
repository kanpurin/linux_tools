#include "../src/gdb.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int wait_stopped(Gdb *g) {
    for (int i = 0; i < 200; ++i) {
        if (gdb_poll(g, 50) < 0) return -1;
        if (g->state == GDB_STOPPED || g->state == GDB_EXITED) return 0;
    }
    return -1;
}

int main(int argc, char **argv) {
    if (argc != 3) return 2;
    Gdb g;
    if (gdb_start(&g, argv[1], NULL)) return 10;
    if (!g.fullname[0] || g.line != 10 || g.state != GDB_NOT_STARTED) return 18;
    if (gdb_toggle_breakpoint(&g, argv[2], 12)) return 11;
    if (gdb_run(&g)) return 12;
    if (!gdb_run(&g) || g.state != GDB_RUNNING ||
        !strstr(g.message, "already running")) return 28;
    if (wait_stopped(&g) || g.state != GDB_STOPPED || g.line != 12) return 12;
    if (!gdb_run(&g) || g.state != GDB_STOPPED ||
        !strstr(g.message, "press c")) return 29;
    char value[128];
    if (gdb_print(&g, "value", value, sizeof(value)) || strcmp(value, "1")) return 13;
    if (gdb_set_cond_breakpoint(&g, argv[2], 14, "value == 2")) return 23;
    int conditional_number = 0;
    for (int i = 0; i < g.break_count; ++i) {
        if (!g.breaks[i].watchpoint && !strcmp(g.breaks[i].condition, "value == 2"))
            conditional_number = g.breaks[i].number;
    }
    if (!conditional_number) return 24;
    if (gdb_delete_breakpoint(&g, conditional_number)) return 25;
    if (gdb_watch(&g, "watched")) return 14;
    int watch_number = 0;
    for (int i = 0; i < g.break_count; ++i) {
        if (g.breaks[i].watchpoint && !strcmp(g.breaks[i].condition, "watched"))
            watch_number = g.breaks[i].number;
    }
    if (!watch_number) return 26;
    if (gdb_continue(&g) || wait_stopped(&g) || g.state != GDB_STOPPED) return 15;
    if (g.stop_breakpoint != watch_number || strcmp(g.watch_old, "1") ||
        strcmp(g.watch_new, "2")) {
        fprintf(stderr, "watch stop mismatch: expected=%d actual=%d old='%s' new='%s' reason='%s'\n",
                watch_number, g.stop_breakpoint, g.watch_old, g.watch_new, g.stop_reason);
        return 27;
    }
    if (gdb_continue(&g) || wait_stopped(&g) || g.state != GDB_EXITED || g.exit_code != 0) return 16;
    if (!strstr(g.output, "value=2")) return 17;
    gdb_shutdown(&g);

    /* finish at main's outermost frame must behave as run-until-exit. */
    if (gdb_start(&g, argv[1], NULL)) return 20;
    if (gdb_toggle_breakpoint(&g, argv[2], 12)) return 21;
    if (gdb_run(&g) || wait_stopped(&g) || g.state != GDB_STOPPED) return 22;
    if (gdb_finish(&g) || wait_stopped(&g) || g.state != GDB_EXITED) return 23;
    gdb_shutdown(&g);
    puts("api smoke: ok");
    return 0;
}
