#include "../src/gdb.h"

#include <stdio.h>
#include <string.h>

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
    if (gdb_toggle_breakpoint(&g, argv[2], 12)) return 11;
    if (gdb_run(&g) || wait_stopped(&g) || g.state != GDB_STOPPED) return 12;

    GdbChild children[8];
    int count = 0;
    if (gdb_list_children(&g, "head", children, 8, &count) || count != 2) return 13;
    if (strcmp(children[0].name, "value") || strcmp(children[0].value, "10")) return 14;
    if (gdb_list_children(&g, "selected", children, 8, &count) || count != 2) return 15;
    if (!strstr(children[0].expression, "selected")) return 16;

    gdb_shutdown(&g);
    puts("children smoke: ok");
    return 0;
}
