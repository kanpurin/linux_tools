#include "stops.h"
#include <stdlib.h>
#include <string.h>

int breakpoint_mark(Gdb *g, const char *file, int line) {
    GdbBreakpoint *b = breakpoint_at(g, file, line);
    if (!b)
        return 0;
    if (!b->enabled)
        return 3;
    return b->condition[0] ? 2 : 1;
}
GdbBreakpoint *breakpoint_at(Gdb *g, const char *file, int line) {
    for (int i = 0; i < g->break_count; i++) {
        GdbBreakpoint *b = &g->breaks[i];
        if (!b->watchpoint && !b->catchpoint && b->line == line &&
            (!strcmp(b->file, file) || strstr(file, b->file)))
            return b;
    }
    return NULL;
}
GdbBreakpoint *breakpoint_at_address(Gdb *g, const char *address) {
    if (!address || !address[0])
        return NULL;
    unsigned long long value = strtoull(address, NULL, 16);
    for (int i = 0; i < g->break_count; i++) {
        GdbBreakpoint *b = &g->breaks[i];
        if (!b->watchpoint && !b->catchpoint && b->address[0] &&
            strtoull(b->address, NULL, 16) == value)
            return b;
    }
    return NULL;
}
int watchpoint_for_expression(Gdb *g, const char *expression) {
    for (int i = 0; i < g->break_count; i++) {
        GdbBreakpoint *b = &g->breaks[i];
        if (b->watchpoint && b->condition[0] && !strcmp(b->condition, expression))
            return b->number;
    }
    return 0;
}
int break_color(const GdbBreakpoint *b) {
    if (!b->enabled)
        return COLOR_MUTED;
    if (b->watchpoint)
        return COLOR_WATCH;
    if (b->catchpoint)
        return COLOR_COND;
    return b->condition[0] ? COLOR_COND : COLOR_BREAK;
}
GdbBreakpoint *breakpoint_by_number(Gdb *g, int number) {
    for (int i = 0; i < g->break_count; i++)
        if (g->breaks[i].number == number)
            return &g->breaks[i];
    return NULL;
}
int newest_break_number(Gdb *g) {
    int n = 0;
    for (int i = 0; i < g->break_count; i++)
        if (g->breaks[i].number > n)
            n = g->breaks[i].number;
    return n;
}
int new_break_line(Gdb *g, const char *file, int previous_max) {
    for (int i = 0; i < g->break_count; i++) {
        GdbBreakpoint *b = &g->breaks[i];
        if (!b->watchpoint && !b->catchpoint && b->number > previous_max && b->line > 0 &&
            (!strcmp(b->file, file) || strstr(file, b->file)))
            return b->line;
    }
    return 0;
}
