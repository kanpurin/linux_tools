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

static int has_var(GdbVar *vars, int count, const char *name) {
    int found = 0;
    for (int i = 0; i < count; ++i) if (!strcmp(vars[i].name, name)) found++;
    return found;
}

int main(int argc, char **argv) {
    if (argc != 3) return 2;
    Gdb g;
    if (gdb_start(&g, argv[1], NULL)) return 10;
    if (gdb_toggle_breakpoint(&g, argv[2], 7)) return 11;
    if (gdb_run(&g) || wait_stopped(&g) || g.state != GDB_STOPPED) return 12;
    if (g.frame_count < 4 || g.selected_frame != 0) return 13;
    if (g.arg_count != 1 || has_var(g.args, g.arg_count, "cursor") != 1 ||
        g.local_count != 0) return 14;

    if (gdb_select_frame(&g, 1) || g.selected_frame != 1) return 15;
    if (g.arg_count != 2 || has_var(g.args, g.arg_count, "dev") != 1 ||
        has_var(g.args, g.arg_count, "value") != 1) return 16;
    if (g.local_count != 1 || has_var(g.locals, g.local_count, "cursor") != 1 ||
        has_var(g.args, g.arg_count, "cursor")) return 17;
    GdbChild children[8];
    int child_count = 0;
    if (gdb_list_children(&g, "dev", children, 8, &child_count) || child_count < 2 ||
        strcmp(children[0].name, "status")) return 18;
    GdbAddressInfo cursor;
    if (gdb_inspect_address(&g, "cursor", &cursor) || !cursor.pointer ||
        strcmp(cursor.points_to, "0")) return 19;

    if (gdb_select_frame(&g, 2) || g.selected_frame != 2) return 20;
    if (g.arg_count != 2 || has_var(g.args, g.arg_count, "dev") != 1 ||
        has_var(g.args, g.arg_count, "value") != 1 || g.local_count != 1 ||
        has_var(g.locals, g.local_count, "adjusted") != 1) return 21;

    if (gdb_select_frame(&g, 3) || g.selected_frame != 3 || g.arg_count != 0 ||
        g.local_count != 1 || has_var(g.locals, g.local_count, "dev") != 1) return 22;
    if (gdb_select_frame(&g, 0) || g.arg_count != 1 ||
        has_var(g.args, g.arg_count, "cursor") != 1 || g.local_count != 0) return 23;

    gdb_shutdown(&g);
    puts("frame variables smoke: ok");
    return 0;
}
