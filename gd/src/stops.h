#ifndef GD_STOPS_H
#define GD_STOPS_H

#include "ui_types.h"

int breakpoint_mark(Gdb *g, const char *file, int line);
GdbBreakpoint *breakpoint_at(Gdb *g, const char *file, int line);
GdbBreakpoint *breakpoint_at_address(Gdb *g, const char *address);
int watchpoint_for_expression(Gdb *g, const char *expression);
int break_color(const GdbBreakpoint *b);
GdbBreakpoint *breakpoint_by_number(Gdb *g, int number);
int newest_break_number(Gdb *g);
int new_break_line(Gdb *g, const char *file, int previous_max);

#endif
