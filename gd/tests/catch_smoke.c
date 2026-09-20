#include "../src/gdb.h"

#include <stdio.h>
#include <string.h>

static int wait_stopped(Gdb *g) {
    for (int i = 0; i < 300; ++i) {
        gdb_poll(g, 50);
        if (g->state == GDB_STOPPED || g->state == GDB_EXITED) return 0;
    }
    return -1;
}

static int find_catch(Gdb *g, const char *type, const char *target) {
    for (int i = 0; i < g->break_count; ++i)
        if (g->breaks[i].catchpoint && !strcmp(g->breaks[i].catch_type, type) &&
            (!target || !strcmp(g->breaks[i].catch_target, target)))
            return g->breaks[i].number;
    return 0;
}

static int has_candidate(char names[][128], int count, const char *name) {
    for (int i = 0; i < count; ++i) if (!strcmp(names[i], name)) return 1;
    return 0;
}

int main(int argc, char **argv) {
    if (argc != 2) return 10;
    Gdb g;
    if (gdb_start(&g, argv[1], NULL)) return 11;
    size_t output_before = g.output_len;
    char names[GD_MAX_CATCH_CANDIDATES][128];
    int count = 0;
    if (gdb_list_catch_candidates(&g, "syscall", "open", names,
                                  GD_MAX_CATCH_CANDIDATES, &count) ||
        !has_candidate(names, count, "openat")) return 12;
    if (gdb_list_catch_candidates(&g, "signal", "SIGUS", names,
                                  GD_MAX_CATCH_CANDIDATES, &count) ||
        !has_candidate(names, count, "SIGUSR1")) return 13;
    if (g.output_len != output_before) return 14;

    const char *events[] = {"fork", "vfork", "exec", "load", "signal"};
    const char *targets[] = {NULL, NULL, NULL, NULL, "SIGUSR1"};
    for (int i = 0; i < 5; ++i) {
        if (gdb_set_catchpoint(&g, events[i], targets[i])) return 20 + i;
        int number = find_catch(&g, events[i], targets[i]);
        if (!number) {
            fprintf(stderr, "missing %s: count=%d message=%s\n", events[i],
                    g.break_count, g.message);
            for (int j = 0; j < g.break_count; ++j)
                fprintf(stderr, "  #%d catch=%d type=%s target=%s original=%s\n",
                        g.breaks[j].number, g.breaks[j].catchpoint,
                        g.breaks[j].catch_type, g.breaks[j].catch_target,
                        g.breaks[j].original_location);
            return 30 + i;
        }
        if (gdb_enable_breakpoint(&g, number, false) ||
            gdb_enable_breakpoint(&g, number, true) ||
            gdb_delete_breakpoint(&g, number)) {
            fprintf(stderr, "manage %s C#%d failed: %s\n", events[i], number,
                    g.message);
            return 30 + i;
        }
    }

    if (gdb_set_catchpoint(&g, "syscall", "openat")) return 40;
    int number = find_catch(&g, "syscall", "openat");
    if (!number) return 41;
    if (gdb_run(&g) || wait_stopped(&g) || g.state != GDB_STOPPED) return 42;
    if (strcmp(g.catch_event, "syscall") || strcmp(g.catch_target, "openat") ||
        strcmp(g.catch_phase, "Entry") || g.stop_breakpoint != number) {
        fprintf(stderr, "entry: event=%s target=%s phase=%s bp=%d message=%s\n",
                g.catch_event, g.catch_target, g.catch_phase, g.stop_breakpoint,
                g.message);
        return 43;
    }
    const char *syscall_regs[] = {"orig_rax", "rdi", "rsi", "rdx", "r10", "r8", "r9"};
    if (g.register_count < 7) return 47;
    for (int i = 0; i < 7; ++i)
        if (strcmp(g.registers[i].name, syscall_regs[i])) return 48;
    if (gdb_continue(&g) || wait_stopped(&g) || g.state != GDB_STOPPED) return 44;
    if (strcmp(g.catch_event, "syscall") || strcmp(g.catch_target, "openat") ||
        strcmp(g.catch_phase, "Return")) return 45;
    if (gdb_delete_breakpoint(&g, number)) return 46;
    gdb_shutdown(&g);
    puts("catch smoke: ok");
    return 0;
}
