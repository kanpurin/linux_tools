#include "../src/gdb.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int wait_stopped(Gdb *g) {
    for (int i = 0; i < 200; ++i) {
        gdb_poll(g, 50);
        if (g->state == GDB_STOPPED || g->state == GDB_EXITED) return 0;
    }
    return -1;
}

static int register_index(Gdb *g, const char *name) {
    for (int i = 0; i < g->register_count; ++i)
        if (!strcmp(g->registers[i].name, name)) return i;
    return -1;
}

int main(int argc, char **argv) {
    if (argc != 2) return 10;
    Gdb g;
    if (gdb_start(&g, argv[1], NULL)) return 11;
    if (g.source_available) return 12;
    GdbFunction functions[32];
    int function_count = 0, found_function = 0;
    if (gdb_list_functions(&g, "process_pay", functions, 32, &function_count))
        return 25;
    for (int i = 0; i < function_count; ++i)
        if (!strcmp(functions[i].name, "process_payment") &&
            functions[i].module[0] && functions[i].address[0]) found_function = 1;
    if (!found_function) return 26;
    if (gdb_set_function_breakpoint(&g, "process_payment")) return 27;
    int function_breakpoint = 0;
    for (int i = 0; i < g.break_count; ++i)
        if (g.breaks[i].function_breakpoint &&
            !strcmp(g.breaks[i].function, "process_payment"))
            function_breakpoint = 1;
    if (!function_breakpoint) return 28;
    if (gdb_run(&g) || wait_stopped(&g) || g.state != GDB_STOPPED) return 13;
    if (g.source_available || g.fullname[0] || g.line != 0) return 14;
    if (g.arg_count || g.local_count) return 15;
    if (!g.pc[0] || g.instruction_count < 2 || g.current_instruction < 0) return 16;
    int rip = register_index(&g, "rip");
    int rdi = register_index(&g, "rdi");
    int r9 = register_index(&g, "r9");
    if (!g.amd64_sysv || rip < 0 || rdi < 0 || r9 < 0 ||
        !g.registers[rip].value[0] || !g.registers[rdi].value[0])
        return 17;
    if (g.frame_count < 2 || !g.frames[0].address[0] ||
        strcmp(g.frames[0].func, "process_payment")) return 18;
    const char *function_address = NULL;
    for (int i = 0; i < g.break_count; ++i)
        if (g.breaks[i].function_breakpoint && g.breaks[i].address[0])
            function_address = g.breaks[i].address;
    if (!function_address || gdb_disassemble_at(&g, function_address) ||
        !g.instruction_count || strcmp(g.disassembly_function, "process_payment"))
        return 29;
    int found_call = 0;
    for (int i = 0; i < g.instruction_count; ++i)
        if (g.instructions[i].call && g.instructions[i].call_target[0]) found_call = 1;
    if (!found_call) return 19;

    int break_instruction = g.current_instruction + 1 < g.instruction_count ?
                            g.current_instruction + 1 : 0;
    const char *address = g.instructions[break_instruction].address;
    if (gdb_toggle_address_breakpoint(&g, address)) {
        fprintf(stderr, "address breakpoint failed: %s\n", g.message);
        return 20;
    }
    int address_break = 0, number = 0;
    for (int i = 0; i < g.break_count; ++i)
        if (g.breaks[i].address[0] &&
            strtoull(g.breaks[i].address, NULL, 16) == strtoull(address, NULL, 16)) {
            address_break = 1;
            number = g.breaks[i].number;
        }
    if (!address_break || !number) {
        fprintf(stderr, "address breakpoint missing: target=%s count=%d\n", address,
                g.break_count);
        for (int i = 0; i < g.break_count; ++i)
            fprintf(stderr, "  #%d addr=%s file=%s line=%d\n", g.breaks[i].number,
                    g.breaks[i].address, g.breaks[i].file, g.breaks[i].line);
        return 21;
    }
    if (gdb_delete_breakpoint(&g, number)) {
        fprintf(stderr, "address breakpoint delete failed: %s\n", g.message);
        return 26;
    }

    char old_pc[128];
    snprintf(old_pc, sizeof(old_pc), "%s", g.pc);
    if (gdb_step_instruction(&g) || wait_stopped(&g) ||
        g.state != GDB_STOPPED || !strcmp(old_pc, g.pc)) return 22;
    if (g.source_available || g.instruction_count < 2 || g.current_instruction < 0)
        return 23;
    if (gdb_next_instruction(&g) || wait_stopped(&g) || g.state != GDB_STOPPED)
        return 24;
    char return_type[256];
    bool is_void = false;
    if (!gdb_current_return_type(&g, return_type, sizeof(return_type), &is_void) ||
        strcmp(return_type, "<unknown>")) return 30;
    if (gdb_force_return(&g, "0") || g.state != GDB_STOPPED ||
        !strcmp(g.function, "process_payment")) {
        fprintf(stderr, "nodebug force return failed: state=%d function=%s message=%s\n",
                g.state, g.function, g.message);
        return 31;
    }
    gdb_shutdown(&g);
    puts("assembly smoke: ok");
    return 0;
}
