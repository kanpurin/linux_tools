#include "app_state.h"
#include "ui.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool app_handle_panes(App *app, int ch) {
    bool stopped = app->g.state == GDB_STOPPED;
    if (app->focus == PANE_VARIABLES) {
        if (ch == 'j' || ch == KEY_DOWN) {
            int next = selectable_var(app->var_rows, app->var_count, app->var_selected + 1, 1);
            if (next >= 0)
                app->var_selected = next;
        } else if (ch == 'k' || ch == KEY_UP) {
            int next = selectable_var(app->var_rows, app->var_count, app->var_selected - 1, -1);
            if (next >= 0)
                app->var_selected = next;
        } else if ((ch == '\n' || ch == KEY_ENTER) && app->var_count) {
            if (stopped)
                toggle_expansion(&app->g, app->expansions, &app->expansion_count,
                                 app->var_rows[app->var_selected].expression);
            else
                snprintf(app->g.message, sizeof(app->g.message), "%s",
                         tui_text("Stop the program before expanding variables",
                                  "変数を展開する前にプログラムを停止してください"));
        } else if (ch == 'p' && app->var_count) {
            if (stopped) {
                char value[GD_TEXT_MAX];
                gdb_print(&app->g, app->var_rows[app->var_selected].expression, value,
                          sizeof(value));
            } else
                snprintf(app->g.message, sizeof(app->g.message), "%s",
                         tui_text("Stop the program before evaluating variables",
                                  "変数を評価する前にプログラムを停止してください"));
        } else if (ch == 'a' && app->var_count) {
            if (stopped) {
                if (!build_address_panel(&app->g, app->var_rows, app->var_count, app->var_selected,
                                         &app->address_panel))
                    app->view = VIEW_ADDRESS;
            } else
                snprintf(app->g.message, sizeof(app->g.message), "%s",
                         tui_text("Stop the program before inspecting addresses",
                                  "アドレスを確認する前にプログラムを停止してください"));
        } else if (ch == 'E' && app->var_count) {
            if (!stopped)
                snprintf(app->g.message, sizeof(app->g.message), "%s",
                         tui_text("Stop the program before modifying a value",
                                  "値を変更する前にプログラムを停止してください"));
            else if (app->var_rows[app->var_selected].header)
                snprintf(app->g.message, sizeof(app->g.message), "%s",
                         tui_text("Select a variable to modify", "変更する変数を選択してください"));
            else if (strstr(app->var_rows[app->var_selected].value, "<optimized out>"))
                snprintf(app->g.message, sizeof(app->g.message), "%s",
                         tui_text("Cannot modify variable: value is optimized out.",
                                  "変数を変更できません: 最適化により値が削除されています。"));
            else {
                memset(&app->edit_dialog, 0, sizeof(app->edit_dialog));
                app->edit_dialog.kind = EDIT_VARIABLE;
                snprintf(app->edit_dialog.expression, sizeof(app->edit_dialog.expression), "%s",
                         app->var_rows[app->var_selected].expression);
                snprintf(app->edit_dialog.label, sizeof(app->edit_dialog.label), "%s",
                         app->var_rows[app->var_selected].expression);
                snprintf(app->edit_dialog.type, sizeof(app->edit_dialog.type), "%s",
                         app->var_rows[app->var_selected].type);
                snprintf(app->edit_dialog.current, sizeof(app->edit_dialog.current), "%s",
                         app->var_rows[app->var_selected].value);
                app->view = VIEW_EDIT_VALUE;
            }
        } else if (ch == 'w' && app->var_count) {
            if (stopped)
                gdb_watch(&app->g, app->var_rows[app->var_selected].expression);
            else
                snprintf(app->g.message, sizeof(app->g.message), "%s",
                         tui_text("Stop the program before setting a watchpoint",
                                  "Watchpointを設定する前にプログラムを停止してください"));
        } else if (ch == 'B' && app->var_count) {
            if (!app->src.path[0])
                snprintf(app->g.message, sizeof(app->g.message), "%s",
                         tui_text("No source location for conditional breakpoint",
                                  "条件付きBreakpointを設定するソース位置がありません"));
            else if (!app->src.debug_lines)
                snprintf(
                    app->g.message, sizeof(app->g.message), "%s",
                    tui_text("Source line breakpoint unavailable: no debug line information.",
                             "ソース行Breakpointを設定できません: デバッグ行情報がありません。"));
            else
                condition_dialog(&app->g, &app->src, &app->cursor,
                                 app->var_rows[app->var_selected].expression, app->var_rows,
                                 app->var_count);
        } else if (ch == 'q') {
            if (app->g.state == GDB_RUNNING || app->g.state == GDB_STOPPED) {
                if (confirm_quit(&app->g))
                    app->done = true;
            } else
                app->done = true;
        } else
            snprintf(app->g.message, sizeof(app->g.message), "%s",
                     tui_text("Variables: Enter expand, p value, a address, w watch, B conditional "
                              "breakpoint",
                              "変数: Enter 展開、p 値、a アドレス、w 監視、B 条件付きBreakpoint"));
        return true;
    }
    if (app->focus == PANE_REGISTERS) {
        if ((ch == 'j' || ch == KEY_DOWN) && app->reg_selected + 1 < app->g.register_count)
            app->reg_selected++;
        else if ((ch == 'k' || ch == KEY_UP) && app->reg_selected > 0)
            app->reg_selected--;
        else if (ch == 'p' && app->g.register_count) {
            build_register_panel(&app->g.registers[app->reg_selected], &app->register_panel);
            app->view = VIEW_REGISTER;
            snprintf(app->g.message, sizeof(app->g.message),
                     tui_text("Register details: %s", "レジスタ詳細: %s"),
                     app->g.registers[app->reg_selected].name);
        } else if (ch == 'E' && app->g.register_count) {
            if (!stopped)
                snprintf(app->g.message, sizeof(app->g.message), "%s",
                         tui_text("Stop the program before modifying a register",
                                  "レジスタを変更する前にプログラムを停止してください"));
            else {
                memset(&app->edit_dialog, 0, sizeof(app->edit_dialog));
                app->edit_dialog.kind = EDIT_REGISTER;
                snprintf(app->edit_dialog.label, sizeof(app->edit_dialog.label), "%s",
                         app->g.registers[app->reg_selected].name);
                snprintf(app->edit_dialog.expression, sizeof(app->edit_dialog.expression), "$%s",
                         app->g.registers[app->reg_selected].name);
                snprintf(app->edit_dialog.current, sizeof(app->edit_dialog.current), "%s",
                         app->g.registers[app->reg_selected].value);
                app->view = VIEW_EDIT_VALUE;
            }
        } else if (ch == 'a')
            snprintf(app->g.message, sizeof(app->g.message), "%s",
                     tui_text("Register memory view is planned after the Assembly MVP",
                              "レジスタのメモリ表示は今後対応予定です"));
        else if (ch == 'q') {
            if (app->g.state == GDB_RUNNING || app->g.state == GDB_STOPPED) {
                if (confirm_quit(&app->g))
                    app->done = true;
            } else
                app->done = true;
        } else
            snprintf(app->g.message, sizeof(app->g.message), "%s",
                     tui_text("Registers: j/k select, p details, Tab pane",
                              "レジスタ: j/k 選択、p 詳細、Tab ペイン切替"));
        return true;
    }
    if (app->focus == PANE_STACK) {
        if ((ch == 'j' || ch == KEY_DOWN) && app->stack_selected + 1 < app->g.frame_count)
            app->stack_selected++;
        else if ((ch == 'k' || ch == KEY_UP) && app->stack_selected > 0)
            app->stack_selected--;
        else if ((ch == '\n' || ch == KEY_ENTER) && app->g.frame_count) {
            GdbFrame frame = app->g.frames[app->stack_selected];
            if (!stopped)
                snprintf(app->g.message, sizeof(app->g.message), "%s",
                         tui_text("Stop the program before selecting a stack frame",
                                  "スタックフレームを選択する前にプログラムを停止してください"));
            else if (!gdb_select_frame(&app->g, frame.level)) {
                app->expansion_count = 0;
                app->var_selected = 0;
                app->var_top = 0;
                app->asm_selected =
                    app->g.current_instruction >= 0 ? app->g.current_instruction : 0;
                app->focus = PANE_SOURCE;
                if (app->g.source_available) {
                    if (source_navigate(&app->src, &app->history, app->g.fullname, app->g.line,
                                        &app->cursor, &app->source_col, &app->top, &app->hscroll,
                                        true, true)) {
                        app->code_view = CODE_SOURCE;
                        snprintf(app->g.message, sizeof(app->g.message),
                                 tui_text("Frame #%d: %s:%d [%s]", "フレーム #%d: %s:%d [%s]"),
                                 frame.level, path_name(app->g.fullname), app->g.line,
                                 project_path(app->g.fullname, app->project_root)
                                     ? tui_text("project", "プロジェクト")
                                     : tui_text("external", "外部"));
                    } else
                        snprintf(app->g.message, sizeof(app->g.message),
                                 tui_text("ERROR: source unavailable: %.900s",
                                          "エラー: ソースを表示できません: %.900s"),
                                 app->g.fullname);
                } else {
                    app->code_view = CODE_ASSEMBLY;
                    history_push_assembly(&app->history, app->g.pc, app->g.function,
                                          app->asm_selected, app->asm_top);
                    snprintf(app->g.message, sizeof(app->g.message),
                             tui_text("Frame #%d: %s @ %s", "フレーム #%d: %s @ %s"), frame.level,
                             app->g.function[0] ? app->g.function : tui_text("<unknown>", "<不明>"),
                             app->g.pc[0]
                                 ? app->g.pc
                                 : tui_text("<address unavailable>", "<アドレス取得不可>"));
                }
            }
        } else if (ch == 'q') {
            if (app->g.state == GDB_RUNNING || app->g.state == GDB_STOPPED) {
                if (confirm_quit(&app->g))
                    app->done = true;
            } else
                app->done = true;
        } else
            snprintf(app->g.message, sizeof(app->g.message), "%s",
                     tui_text("Stack: j/k select, Enter selects frame and opens its source",
                              "スタック: j/k 選択、Enterでフレーム選択とソース移動"));
        return true;
    }
    return false;
}
