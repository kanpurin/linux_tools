#ifndef GD_VARIABLES_H
#define GD_VARIABLES_H

#include "ui_types.h"

int build_var_rows(Gdb *g, VarExpansion *expansions, int expansion_count, VarRow *rows);
int selectable_var(VarRow *rows, int count, int start, int direction);
void toggle_expansion(Gdb *g, VarExpansion *items, int *count, const char *expression);
void refresh_expansions(Gdb *g, VarExpansion *items, int *count);
int build_address_panel(Gdb *g, VarRow *rows, int row_count, int selected, AddressPanel *panel);

#endif
