#include "app_state.h"
#include "ui.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void app_handle_input(App *app, int ch) {
    bool stopped = app->g.state == GDB_STOPPED;
    /* Handle search cancellation before pane, mode and Assembly dispatch. */
    if (ch == 27) {
        app->search_active = false;
        app->last_search[0] = '\0';
        app->search_whole_word = false;
        app->search_direction = 1;
        app->vim_count = 0;
        snprintf(app->g.message, sizeof(app->g.message), "%s",
                 app->mode == MODE_GDB
                     ? tui_text("Search cleared; n is debugger next",
                                "検索を解除しました。nはデバッガーのnextです")
                     : tui_text("Search and highlights cleared", "検索とハイライトを解除しました"));
        return;
    }
    if (ch == 7) {
        project_search(&app->g, app->project_root, &app->src, &app->history, &app->cursor,
                       &app->source_col, &app->top, &app->hscroll, &app->code_view, &app->focus,
                       app->last_search, sizeof(app->last_search), &app->search_active,
                       &app->search_whole_word);
        return;
    }
    if (ch == '\t') {
        app->focus = cycle_focus(app->focus, app->code_view, 1);
        snprintf(app->g.message, sizeof(app->g.message), tui_text("Focus: %s", "フォーカス: %s"),
                 focus_name(app->focus));
        return;
    }
    if (ch == KEY_BTAB) {
        app->focus = cycle_focus(app->focus, app->code_view, -1);
        snprintf(app->g.message, sizeof(app->g.message), tui_text("Focus: %s", "フォーカス: %s"),
                 focus_name(app->focus));
        return;
    }
    if (ch == KEY_F(2)) {
        app->mode = app->mode == MODE_VIM ? MODE_GDB : MODE_VIM;
        app->search_active = false;
        app->vim_count = 0;
        snprintf(app->g.message, sizeof(app->g.message), "%s",
                 app->mode == MODE_VIM ? tui_text("VIM navigation mode", "VIM操作モード")
                                       : tui_text("GDB control mode", "GDB操作モード"));
        return;
    }
    if (app->mode == MODE_GDB && ch == 'L') {
        if (!ui_is_japanese() && !locale_is_utf8())
            snprintf(app->g.message, sizeof(app->g.message), "Japanese UI requires a UTF-8 locale");
        else {
            ui_set_japanese(!ui_is_japanese());
            app->g.japanese = ui_is_japanese();
            snprintf(
                app->g.message, sizeof(app->g.message), "%s",
                tui_text("Display language changed to English", "表示言語を日本語に変更しました"));
        }
        return;
    }
    if (ch == '?' && app->mode == MODE_GDB) {
        app->view = VIEW_HELP;
        return;
    }
    if (ch == 'C') {
        app->catch_selected = 0;
        app->view = VIEW_CATCH_MENU;
        return;
    }
    if (ch == 'F') {
        memset(&app->function_dialog, 0, sizeof(app->function_dialog));
        app->function_dialog.action = FUNCTION_BREAKPOINT;
        refresh_function_dialog(&app->g, &app->function_dialog);
        app->view = VIEW_FUNCTIONS;
        return;
    }
    if (ch == 'R') {
        if (app->g.state != GDB_STOPPED)
            snprintf(app->g.message, sizeof(app->g.message), "%s",
                     tui_text("Stop the program before forcing a return",
                              "強制returnの前にプログラムを停止してください"));
        else {
            memset(&app->force_return_dialog, 0, sizeof(app->force_return_dialog));
            snprintf(app->force_return_dialog.function, sizeof(app->force_return_dialog.function),
                     "%s", app->g.function[0] ? app->g.function : tui_text("<unknown>", "<不明>"));
            app->force_return_dialog.type_known = !gdb_current_return_type(
                &app->g, app->force_return_dialog.return_type,
                sizeof(app->force_return_dialog.return_type), &app->force_return_dialog.is_void);
            snprintf(app->g.message, sizeof(app->g.message),
                     tui_text("Force return from %s()", "%s()から強制return"),
                     app->force_return_dialog.function);
            app->view = VIEW_FORCE_RETURN;
        }
        return;
    }
    if (ch == 'o') {
        app->open_selected = 0;
        app->view = VIEW_OPEN_MENU;
        return;
    }
    if (app->mode == MODE_GDB && ch == 'e') {
        if (app->g.source_available && app->g.fullname[0] &&
            source_navigate(&app->src, &app->history, app->g.fullname, app->g.line, &app->cursor,
                            &app->source_col, &app->top, &app->hscroll, true, true)) {
            app->code_view = CODE_SOURCE;
            app->focus = PANE_SOURCE;
            snprintf(app->g.message, sizeof(app->g.message),
                     tui_text("Execution position: %s:%d", "実行位置: %s:%d"),
                     path_name(app->g.fullname), app->g.line);
        } else if (app->g.pc[0] && !gdb_disassemble_at(&app->g, app->g.pc)) {
            app->code_view = CODE_ASSEMBLY;
            app->focus = PANE_SOURCE;
            app->asm_selected = app->g.current_instruction >= 0 ? app->g.current_instruction : 0;
            app->asm_top =
                app->asm_selected > app->source_h / 2 ? app->asm_selected - app->source_h / 2 : 0;
            history_push_assembly(&app->history, app->g.pc, app->g.function, app->asm_selected,
                                  app->asm_top);
            snprintf(app->g.message, sizeof(app->g.message),
                     tui_text("Execution position: %s @ %s", "実行位置: %s @ %s"),
                     app->g.function[0] ? app->g.function : tui_text("<unknown>", "<不明>"),
                     app->g.pc);
        } else
            snprintf(app->g.message, sizeof(app->g.message), "%s",
                     tui_text("Execution position is unavailable", "実行位置を取得できません"));
        return;
    }
    if (ch == 'd') {
        if (app->code_view == CODE_SOURCE) {
            if (app->g.pc[0] && !gdb_disassemble_at(&app->g, app->g.pc)) {
                app->code_view = CODE_ASSEMBLY;
                app->asm_selected =
                    app->g.current_instruction >= 0 ? app->g.current_instruction : 0;
                history_push_assembly(&app->history, app->g.pc, app->g.function, app->asm_selected,
                                      app->asm_top);
            }
            if (app->focus == PANE_VARIABLES)
                app->focus = PANE_REGISTERS;
            snprintf(app->g.message, sizeof(app->g.message), "%s",
                     tui_text("Assembly view", "Assembly表示"));
        } else if (app->src.count) {
            app->code_view = CODE_SOURCE;
            if (app->focus == PANE_REGISTERS)
                app->focus = PANE_VARIABLES;
            history_push(&app->history, app->src.path, app->cursor, app->source_col, app->top,
                         app->hscroll, app->src.debug_lines);
            snprintf(app->g.message, sizeof(app->g.message), "%s",
                     app->src.debug_lines
                         ? tui_text("Source view", "Source表示")
                         : tui_text("Source view (REFERENCE ONLY)", "Source表示（参照のみ）"));
        } else
            snprintf(app->g.message, sizeof(app->g.message), "%s",
                     tui_text("No source file is available; remaining in Assembly view",
                              "ソースファイルがないためAssembly表示を継続します"));
        return;
    }
    if (app->code_view == CODE_ASSEMBLY && (ch == 'i' || ch == 'I')) {
        if (app->g.state != GDB_STOPPED)
            snprintf(app->g.message, sizeof(app->g.message), "%s",
                     tui_text("Stop the program before instruction stepping",
                              "命令単位で実行する前にプログラムを停止してください"));
        else if (ch == 'i')
            gdb_step_instruction(&app->g);
        else
            gdb_next_instruction(&app->g);
        return;
    }
    if (ch == '[' || ch == ']' || ch == 15) {
        int direction = ch == ']' ? 1 : -1;
        if (history_move(&app->history, &app->src, &app->g, direction, &app->code_view,
                         &app->cursor, &app->source_col, &app->top, &app->hscroll,
                         &app->asm_selected, &app->asm_top)) {
            app->focus = PANE_SOURCE;
            if (app->code_view == CODE_SOURCE)
                snprintf(app->g.message, sizeof(app->g.message),
                         tui_text("View history %d/%d: %s:%d", "閲覧履歴 %d/%d: %s:%d"),
                         app->history.index + 1, app->history.count, path_name(app->src.path),
                         app->cursor + 1);
            else
                snprintf(app->g.message, sizeof(app->g.message),
                         tui_text("View history %d/%d: Disassembly %s",
                                  "閲覧履歴 %d/%d: 逆アセンブル %s"),
                         app->history.index + 1, app->history.count,
                         app->g.disassembly_function[0] ? app->g.disassembly_function : app->g.pc);
        } else
            snprintf(app->g.message, sizeof(app->g.message), "%s",
                     direction < 0
                         ? tui_text("No older view location", "これより前の閲覧履歴はありません")
                         : tui_text("No newer view location", "これより後の閲覧履歴はありません"));
        return;
    }
    char input[1024];
    if (app_handle_panes(app, ch) || app_handle_code(app, ch))
        return;
    if (app->search_active && (ch == 'n' || ch == 'N')) {
        int found = source_search(&app->src, app->cursor, app->last_search, ch == 'n' ? 1 : -1);
        if (found >= 0) {
            app->cursor = found;
            app->source_col =
                text_column(app->src.lines[app->cursor], app->last_search, ch == 'n' ? 1 : -1);
            snprintf(app->g.message, sizeof(app->g.message),
                     tui_text("Search '%.*s': line %d  [n next / N previous / Esc end]",
                              "検索 '%.*s': %d行目  [n 次 / N 前 / Esc 終了]"),
                     500, app->last_search, found + 1);
        } else
            snprintf(app->g.message, sizeof(app->g.message),
                     tui_text("Search text not found: %.*s  [Esc end]",
                              "検索文字列が見つかりません: %.*s  [Esc 終了]"),
                     700, app->last_search);
        return;
    }
    if (app->search_active && ch != '/' && ch != 'j' && ch != 'k' && ch != KEY_UP &&
        ch != KEY_DOWN && ch != KEY_NPAGE && ch != KEY_PPAGE && ch != 4 && ch != 21 && ch != 'g' &&
        ch != 'G')
        app->search_active = false;
    if (ch == 'j' || ch == KEY_DOWN) {
        if (app->cursor + 1 < app->src.count)
            app->cursor++;
    } else if (ch == 'k' || ch == KEY_UP) {
        if (app->cursor > 0)
            app->cursor--;
    } else if (ch == KEY_NPAGE) {
        if (app->src.count) {
            app->cursor += app->source_h > 1 ? app->source_h - 1 : 1;
            if (app->cursor >= app->src.count)
                app->cursor = app->src.count - 1;
        }
    } else if (ch == KEY_PPAGE) {
        if (app->src.count) {
            app->cursor -= app->source_h > 1 ? app->source_h - 1 : 1;
            if (app->cursor < 0)
                app->cursor = 0;
        }
    } else if (ch == 4) {
        if (app->src.count) {
            app->cursor += app->source_h / 2 > 0 ? app->source_h / 2 : 1;
            if (app->cursor >= app->src.count)
                app->cursor = app->src.count - 1;
        }
    } else if (ch == 21) {
        if (app->src.count) {
            app->cursor -= app->source_h / 2 > 0 ? app->source_h / 2 : 1;
            if (app->cursor < 0)
                app->cursor = 0;
        }
    } else if (ch == 'g')
        app->cursor = 0;
    else if (ch == 'G')
        app->cursor = app->src.count ? app->src.count - 1 : 0;
    else if (ch == '/') {
        input[0] = 0;
        if (prompt_text(tui_text("Search: ", "検索: "), input, sizeof(input))) {
            snprintf(app->last_search, sizeof(app->last_search), "%s", input);
            app->search_active = true;
            app->search_whole_word = false;
            int found = source_search(&app->src, app->cursor, app->last_search, 1);
            if (found >= 0) {
                app->cursor = found;
                app->source_col = text_column(app->src.lines[app->cursor], app->last_search, 1);
                snprintf(app->g.message, sizeof(app->g.message),
                         tui_text("Search '%.*s': line %d  [n next / N previous / Esc end]",
                                  "検索 '%.*s': %d行目  [n 次 / N 前 / Esc 終了]"),
                         500, app->last_search, found + 1);
            } else
                snprintf(app->g.message, sizeof(app->g.message),
                         tui_text("Search text not found: %.*s  [n/N retry / Esc end]",
                                  "検索文字列が見つかりません: %.*s  [n/N 再検索 / Esc 終了]"),
                         650, app->last_search);
        }
    } else if (ch == 'r')
        gdb_run(&app->g);
    else if (ch == 'c' && stopped)
        gdb_continue(&app->g);
    else if (ch == 'n' && stopped)
        gdb_next(&app->g);
    else if (ch == 's' && stopped)
        gdb_step(&app->g);
    else if (ch == 'f' && stopped)
        gdb_finish(&app->g);
    else if (ch == 'b' && app->src.path[0]) {
        if (!app->src.debug_lines)
            snprintf(app->g.message, sizeof(app->g.message), "%s",
                     tui_text("Source line breakpoint unavailable: no debug line information.",
                              "ソース行Breakpointを設定できません: デバッグ行情報がありません。"));
        else {
            int marked = breakpoint_mark(&app->g, app->src.path, app->cursor + 1),
                oldmax = newest_break_number(&app->g), requested = app->cursor + 1;
            if (!gdb_toggle_breakpoint(&app->g, app->src.path, requested) && !marked) {
                int actual = new_break_line(&app->g, app->src.path, oldmax);
                if (actual > 0) {
                    app->cursor = actual - 1;
                    if (actual != requested)
                        snprintf(app->g.message, sizeof(app->g.message),
                                 tui_text("Breakpoint moved to executable line %d",
                                          "Breakpointを実行可能な%d行目へ移動しました"),
                                 actual);
                }
            }
        }
    } else if (ch == 'B' && app->src.path[0]) {
        if (!app->src.debug_lines)
            snprintf(app->g.message, sizeof(app->g.message), "%s",
                     tui_text("Source line breakpoint unavailable: no debug line information.",
                              "ソース行Breakpointを設定できません: デバッグ行情報がありません。"));
        else
            condition_dialog(&app->g, &app->src, &app->cursor, "", app->var_rows, app->var_count);
    } else if (ch == 'w' && stopped) {
        input[0] = 0;
        if (prompt_text(tui_text("Watch variable/expression: ", "監視する変数／式: "), input,
                        sizeof(input)))
            gdb_watch(&app->g, input);
    } else if (ch == 'p' && stopped) {
        input[0] = 0;
        if (prompt_text(tui_text("Print expression: ", "評価する式: "), input, sizeof(input))) {
            char value[GD_TEXT_MAX];
            gdb_print(&app->g, input, value, sizeof(value));
        }
    } else if (ch == 'S') {
        gdb_refresh_breakpoints(&app->g);
        app->view = VIEW_BREAKS;
    } else if (ch == 'O')
        app->view = VIEW_OUTPUT;
    else if (ch == 'h')
        app->view = VIEW_HELP;
    else if (ch == 18 && app->src.path[0]) {
        app->src.mtime = 0;
        source_load(&app->src, app->src.path);
        snprintf(app->g.message, sizeof(app->g.message), "%s",
                 tui_text("Source reloaded", "ソースを再読込みしました"));
    } else if (ch == 'q') {
        if (app->g.state == GDB_RUNNING || app->g.state == GDB_STOPPED) {
            if (confirm_quit(&app->g))
                app->done = true;
        } else
            app->done = true;
    }
}
