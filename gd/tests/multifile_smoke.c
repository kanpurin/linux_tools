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
    if (argc != 4) return 2;
    Gdb g;
    if (gdb_start(&g, argv[1], NULL)) return 10;
    if (strcmp(g.fullname, argv[2])) return 11;
    GdbSourceFile files[32];
    int file_count = 0, main_source = 0, helper_source = 0;
    if (gdb_list_source_files(&g, files, 32, &file_count)) return 19;
    for (int i = 0; i < file_count; ++i) {
        if (!strcmp(files[i].fullname, argv[2])) main_source = 1;
        if (!strcmp(files[i].fullname, argv[3])) helper_source = 1;
    }
    if (!main_source || !helper_source) return 20;
    GdbFunction functions[16];
    int function_count = 0, helper_function = 0;
    if (gdb_list_functions(&g, "helper", functions, 16, &function_count)) return 21;
    for (int i = 0; i < function_count; ++i)
        if (!strcmp(functions[i].name, "helper") &&
            !strcmp(functions[i].source, argv[3]) && functions[i].line == 1)
            helper_function = 1;
    if (!helper_function) return 22;
    if (gdb_toggle_breakpoint(&g, argv[2], 5)) return 12;
    if (gdb_run(&g) || wait_stopped(&g) || g.state != GDB_STOPPED ||
        strcmp(g.fullname, argv[2]) || g.line != 5) return 13;
    if (gdb_step(&g) || wait_stopped(&g) || g.state != GDB_STOPPED ||
        strcmp(g.fullname, argv[3]) || g.line != 2) return 14;
    if (g.frame_count < 2 || strcmp(g.frames[0].file, argv[3]) ||
        strcmp(g.frames[1].file, argv[2])) return 15;
    if (gdb_select_frame(&g, g.frames[1].level) || g.selected_frame != 1 ||
        strcmp(g.fullname, argv[2]) || g.line != 5) return 16;
    char value[64];
    if (gdb_print(&g, "value", value, sizeof(value)) || strcmp(value, "3")) return 17;
    if (gdb_select_frame(&g, 0) || g.selected_frame != 0 ||
        strcmp(g.fullname, argv[3]) || g.line != 2) return 18;
    gdb_shutdown(&g);
    puts("multifile smoke: ok");
    return 0;
}
