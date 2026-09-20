#include "../src/gdb.h"

#include <stdbool.h>
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
    if (gdb_toggle_breakpoint(&g, argv[2], 4)) return 12;
    if (gdb_run(&g) || wait_stopped(&g) || g.state != GDB_STOPPED) return 13;
    if (strcmp(g.function, "mutation_worker")) return 14;

    char actual[GD_TEXT_MAX], type[256];
    bool is_void = false;
    if (gdb_assign_expression(&g, "value", "10", actual, sizeof(actual)) ||
        strcmp(actual, "10")) return 15;
    if (gdb_assign_expression(&g, "dev->status", "7", actual, sizeof(actual)) ||
        strcmp(actual, "7")) return 16;
    if (gdb_assign_register(&g, "rax", "0x123", actual, sizeof(actual)) ||
        (strcmp(actual, "291") && !strstr(actual, "0x123"))) {
        fprintf(stderr, "register edit failed: actual=%s message=%s\n", actual,
                g.message);
        return 17;
    }
    if (gdb_current_return_type(&g, type, sizeof(type), &is_void) || is_void ||
        strcmp(type, "int")) {
        fprintf(stderr, "return type failed: type=%s message=%s\n", type, g.message);
        return 18;
    }
    if (gdb_force_return(&g, "42")) {
        fprintf(stderr, "force return failed: %s\n", g.message);
        return 19;
    }
    if (g.state != GDB_STOPPED || strcmp(g.function, "main") || !g.source_available)
        return 20;
    gdb_shutdown(&g);

    Gdb void_g;
    if (gdb_start(&void_g, argv[1], NULL)) return 21;
    if (gdb_set_function_breakpoint(&void_g, "mutation_void") ||
        gdb_run(&void_g) || wait_stopped(&void_g) ||
        void_g.state != GDB_STOPPED) return 22;
    is_void = false;
    if (gdb_current_return_type(&void_g, type, sizeof(type), &is_void) ||
        !is_void || strcmp(type, "void")) return 23;
    if (gdb_force_return(&void_g, NULL) || void_g.state != GDB_STOPPED ||
        strcmp(void_g.function, "main")) return 24;
    gdb_shutdown(&void_g);
    puts("mutation smoke: ok");
    return 0;
}
