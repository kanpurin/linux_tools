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
    if (gdb_toggle_breakpoint(&g, argv[2], 14)) return 11;
    if (gdb_run(&g) || wait_stopped(&g) || g.state != GDB_STOPPED) return 12;

    GdbAddressInfo cursor, value, null_ptr, bad_ptr;
    if (gdb_inspect_address(&g, "cursor", &cursor) || !cursor.pointer ||
        !cursor.address_available || cursor.is_null ||
        !cursor.points_to_available || strcmp(cursor.points_to, "7")) return 13;
    char value_address[128];
    if (gdb_expression_address(&g, "value", value_address, sizeof(value_address)) ||
        strcmp(cursor.address, value_address)) return 14;
    if (gdb_inspect_address(&g, "value", &value) || value.pointer ||
        !value.address_available || strcmp(value.value, "7") ||
        strcmp(value.address, cursor.address)) return 15;
    if (gdb_inspect_address(&g, "null_ptr", &null_ptr) || !null_ptr.pointer ||
        !null_ptr.is_null || strcmp(null_ptr.address, "0x0") ||
        strcmp(null_ptr.points_to, "NULL")) return 16;
    if (gdb_inspect_address(&g, "bad_ptr", &bad_ptr) || !bad_ptr.pointer ||
        bad_ptr.is_null || bad_ptr.points_to_available ||
        strcmp(bad_ptr.points_to, "<unavailable>")) return 17;

    GdbAddressInfo member_cursor;
    if (gdb_inspect_address(&g, "member_cursor", &member_cursor) ||
        !member_cursor.pointer || strcmp(member_cursor.points_to, "9")) return 18;
    GdbChild children[8];
    int child_count = 0, matched_member = 0;
    if (gdb_list_children(&g, "holder_ptr", children, 8, &child_count)) return 19;
    for (int i = 0; i < child_count; ++i) {
        if (strcmp(children[i].name, "target")) continue;
        char member_address[128];
        if (!gdb_expression_address(&g, children[i].expression, member_address,
                                    sizeof(member_address)) &&
            !strcmp(member_address, member_cursor.address)) matched_member = 1;
    }
    if (!matched_member) return 20;

    gdb_shutdown(&g);
    puts("address smoke: ok");
    return 0;
}
