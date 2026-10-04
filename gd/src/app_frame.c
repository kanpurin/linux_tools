#include "app_state.h"
#include "ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void app_update_and_draw(App *app) {
    GdbState state_before = app->g.state;
    int line_before = app->g.line;
    unsigned long output_before = app->g.output_revision;
    char file_before[GD_PATH_MAX], pc_before[128];
    snprintf(file_before, sizeof(file_before), "%s", app->g.fullname);
    snprintf(pc_before, sizeof(pc_before), "%s", app->g.pc);
    app->g.changed = false;
    gdb_poll(&app->g, 0);
    int rows, cols;
    getmaxyx(stdscr, rows, cols);
    if (!app->redraw_requested && !app->g.changed && app->g.state == state_before &&
        app->g.output_revision == output_before && rows == app->rows && cols == app->cols)
        return;
    app->redraw_requested = false;
    app->draw_count++;
    bool stopped_now = app->g.state == GDB_STOPPED &&
                       (state_before != GDB_STOPPED || app->g.line != line_before ||
                        strcmp(app->g.fullname, file_before) || strcmp(app->g.pc, pc_before));
    if (stopped_now) {
        app->expansion_count = 0;
        app->var_selected = 0;
        app->var_top = 0;
        app->stack_selected = 0;
        if (!strcmp(app->g.catch_event, "exec")) {
            source_free(&app->src);
            memset(&app->history, 0, sizeof(app->history));
            app->history.index = -1;
            project_root_init(&app->g, app->project_root, sizeof(app->project_root));
        }
        if (app->g.source_available) {
            app->code_view = CODE_SOURCE;
            if (app->focus == PANE_REGISTERS)
                app->focus = PANE_VARIABLES;
        } else {
            app->code_view = CODE_ASSEMBLY;
            if (app->focus == PANE_VARIABLES)
                app->focus = PANE_REGISTERS;
        }
        app->asm_selected = app->g.current_instruction >= 0 ? app->g.current_instruction : 0;
    }
    if (!app->src.count && app->g.source_available) {
        if (!source_navigate(&app->src, &app->history, app->g.fullname, app->g.line, &app->cursor,
                             &app->source_col, &app->top, &app->hscroll, true, true)) {
            app->code_view = CODE_ASSEMBLY;
            if (app->focus == PANE_VARIABLES)
                app->focus = PANE_REGISTERS;
            snprintf(app->g.message, sizeof(app->g.message),
                     tui_text("Source file unavailable; using Assembly: %.850s",
                              "ソースを表示できないためAssemblyを使用します: %.850s"),
                     app->g.fullname);
        }
    } else if (stopped_now && app->g.source_available) {
        if (!same_source_path(app->src.path, app->g.fullname)) {
            if (!source_navigate(&app->src, &app->history, app->g.fullname, app->g.line,
                                 &app->cursor, &app->source_col, &app->top, &app->hscroll, true,
                                 true)) {
                app->code_view = CODE_ASSEMBLY;
                if (app->focus == PANE_VARIABLES)
                    app->focus = PANE_REGISTERS;
                snprintf(app->g.message, sizeof(app->g.message),
                         tui_text("Source file unavailable; using Assembly: %.850s",
                                  "ソースを表示できないためAssemblyを使用します: %.850s"),
                         app->g.fullname);
            }
        } else {
            app->src.debug_lines = true;
            app->cursor = app->g.line - 1;
            app->source_col = 0;
            app->hscroll = 0;
            app->top = app->cursor;
        }
    }
    clamp_column(&app->src, app->cursor, &app->source_col);
    getmaxyx(stdscr, app->rows, app->cols);
    layout_heights(app->rows, &app->source_h, &app->variables_h, &app->stack_h);
    if (app->code_view == CODE_ASSEMBLY)
        while (app->variables_h < 6 && app->stack_h > 2) {
            app->variables_h++;
            app->stack_h--;
        }
    if (stopped_now && app->code_view == CODE_SOURCE && app->g.source_available && app->src.count &&
        same_source_path(app->src.path, app->g.fullname))
        app->top = execution_context_top(app->cursor, app->source_h);
    int text_width = app->cols - 18;
    if (app->code_view == CODE_ASSEMBLY && !app->g.instruction_count) {
        gdb_ensure_disassembly(&app->g);
        app->asm_selected = app->g.current_instruction >= 0 ? app->g.current_instruction : 0;
        app->asm_top =
            app->asm_selected > app->source_h / 2 ? app->asm_selected - app->source_h / 2 : 0;
    }
    if (app->cursor < app->top)
        app->top = app->cursor;
    if (app->cursor >= app->top + app->source_h)
        app->top = app->cursor - app->source_h + 1;
    if (app->top < 0)
        app->top = 0;
    if (app->source_col < app->hscroll)
        app->hscroll = app->source_col;
    if (text_width > 0 && app->source_col >= app->hscroll + text_width)
        app->hscroll = app->source_col - text_width + 1;
    if (app->hscroll < 0)
        app->hscroll = 0;
    if (app->code_view == CODE_SOURCE)
        history_update(&app->history, &app->src, app->cursor, app->source_col, app->top,
                       app->hscroll);
    else
        history_update_assembly(&app->history, app->asm_selected, app->asm_top);
    app->var_count = build_var_rows(&app->g, app->expansions, app->expansion_count, app->var_rows);
    if (!app->var_count)
        app->var_selected = 0;
    else if (app->var_selected >= app->var_count || app->var_rows[app->var_selected].header) {
        int start = app->var_selected < app->var_count ? app->var_selected : 0;
        int selectable = selectable_var(app->var_rows, app->var_count, start, 1);
        if (selectable < 0)
            selectable = selectable_var(app->var_rows, app->var_count, start - 1, -1);
        app->var_selected = selectable >= 0 ? selectable : 0;
    }
    if (app->var_selected < app->var_top)
        app->var_top = app->var_selected;
    if (app->var_selected >= app->var_top + app->variables_h)
        app->var_top = app->var_selected - app->variables_h + 1;
    if (app->reg_selected >= app->g.register_count)
        app->reg_selected = app->g.register_count ? app->g.register_count - 1 : 0;
    if (app->reg_selected < app->reg_top)
        app->reg_top = app->reg_selected;
    if (app->reg_selected >= app->reg_top + app->variables_h)
        app->reg_top = app->reg_selected - app->variables_h + 1;
    if (app->asm_selected >= app->g.instruction_count)
        app->asm_selected = app->g.instruction_count ? app->g.instruction_count - 1 : 0;
    if (app->asm_selected < app->asm_top)
        app->asm_top = app->asm_selected;
    if (app->asm_selected >= app->asm_top + app->source_h)
        app->asm_top = app->asm_selected - app->source_h + 1;
    if (stopped_now && app->g.current_instruction >= 0) {
        app->asm_selected = app->g.current_instruction;
        app->asm_top = app->asm_selected - app->source_h / 2;
        if (app->asm_top < 0)
            app->asm_top = 0;
    }
    if (stopped_now) {
        if (app->code_view == CODE_SOURCE && app->g.source_available && app->src.count &&
            same_source_path(app->src.path, app->g.fullname))
            history_push(&app->history, app->src.path, app->cursor, app->source_col, app->top,
                         app->hscroll, true);
        else if (app->g.pc[0])
            history_push_assembly(&app->history, app->g.pc, app->g.function, app->asm_selected,
                                  app->asm_top);
    }
    if (app->stack_selected >= app->g.frame_count)
        app->stack_selected = app->g.frame_count ? app->g.frame_count - 1 : 0;
    if (app->view == VIEW_MAIN) {
        draw_main(&app->g, &app->src, app->cursor, app->source_col, app->top, app->hscroll,
                  app->mode, app->code_view, app->focus, app->var_rows, app->var_count,
                  app->var_top, app->var_selected, app->reg_selected, app->reg_top,
                  app->asm_selected, app->asm_top, app->stack_selected, app->last_search,
                  app->search_whole_word, app->project_root, app->history.index,
                  app->history.count);
        refresh();
    } else if (app->view == VIEW_HELP) {
        draw_main(&app->g, &app->src, app->cursor, app->source_col, app->top, app->hscroll,
                  app->mode, app->code_view, app->focus, app->var_rows, app->var_count,
                  app->var_top, app->var_selected, app->reg_selected, app->reg_top,
                  app->asm_selected, app->asm_top, app->stack_selected, app->last_search,
                  app->search_whole_word, app->project_root, app->history.index,
                  app->history.count);
        wnoutrefresh(stdscr);
        draw_help_popup();
        doupdate();
    } else if (app->view == VIEW_OUTPUT)
        draw_output(&app->g);
    else if (app->view == VIEW_ADDRESS)
        draw_address(&app->address_panel);
    else if (app->view == VIEW_REGISTER)
        draw_register_panel(&app->register_panel);
    else if (app->view == VIEW_FUNCTIONS)
        draw_function_dialog(&app->g, &app->function_dialog);
    else if (app->view == VIEW_OPEN_MENU)
        draw_open_menu(app->open_selected);
    else if (app->view == VIEW_OPEN_FILES)
        draw_file_dialog(&app->g, &app->file_dialog);
    else if (app->view == VIEW_EDIT_VALUE)
        draw_edit_dialog(&app->edit_dialog);
    else if (app->view == VIEW_FORCE_RETURN)
        draw_force_return_dialog(&app->force_return_dialog);
    else if (app->view == VIEW_CATCH_MENU)
        draw_catch_menu(app->catch_selected);
    else if (app->view == VIEW_CATCH_SEARCH)
        draw_catch_search(&app->g, &app->catch_dialog);
    else
        draw_breaks(&app->g, app->bsel, app->project_root);
}
