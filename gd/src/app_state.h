#ifndef GD_APP_STATE_H
#define GD_APP_STATE_H

#include "ui_types.h"

/* UI owns its browsing and selection state; GDB owns the inferior state. */
typedef struct {
    Gdb g;
    Source src;
    SourceHistory history;
    char project_root[GD_PATH_MAX];
    View view;
    InputMode mode;
    CodeView code_view;
    PaneFocus focus;
    int cursor, source_col, top, hscroll, bsel;
    int var_selected, var_top, reg_selected, reg_top;
    int asm_selected, asm_top, stack_selected;
    int vim_count, search_direction, expansion_count;
    VarExpansion expansions[UI_MAX_EXPANSIONS];
    VarRow var_rows[UI_MAX_VAR_ROWS];
    AddressPanel address_panel;
    RegisterPanel register_panel;
    FunctionDialog function_dialog;
    FileDialog file_dialog;
    EditDialog edit_dialog;
    ForceReturnDialog force_return_dialog;
    CatchDialog catch_dialog;
    int open_selected, catch_selected;
    char last_search[1024];
    bool search_active, search_whole_word, done;
    bool redraw_requested;
    unsigned long draw_count;
    int rows, cols, source_h, variables_h, stack_h, var_count;
} App;

void app_update_and_draw(App *app);
bool app_handle_view(App *app, int ch);
void app_handle_input(App *app, int ch);
bool app_handle_panes(App *app, int ch);
bool app_handle_code(App *app, int ch);

#endif
