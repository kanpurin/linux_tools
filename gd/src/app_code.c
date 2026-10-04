#include "app_state.h"
#include "ui.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool app_handle_code(App *app, int ch) {
    bool stopped = app->g.state == GDB_STOPPED;
    if (app->code_view == CODE_ASSEMBLY) {
        if ((ch == 'j' || ch == KEY_DOWN) && app->asm_selected + 1 < app->g.instruction_count)
            app->asm_selected++;
        else if ((ch == 'k' || ch == KEY_UP) && app->asm_selected > 0)
            app->asm_selected--;
        else if (ch == KEY_NPAGE) {
            app->asm_selected += app->source_h > 1 ? app->source_h - 1 : 1;
            if (app->asm_selected >= app->g.instruction_count)
                app->asm_selected = app->g.instruction_count ? app->g.instruction_count - 1 : 0;
        } else if (ch == KEY_PPAGE) {
            app->asm_selected -= app->source_h > 1 ? app->source_h - 1 : 1;
            if (app->asm_selected < 0)
                app->asm_selected = 0;
        } else if (ch == 'b' && app->g.instruction_count) {
            if (stopped)
                gdb_toggle_address_breakpoint(&app->g,
                                              app->g.instructions[app->asm_selected].address);
            else
                snprintf(app->g.message, sizeof(app->g.message), "%s",
                         tui_text("Stop the program before setting an address breakpoint",
                                  "アドレスBreakpointを設定する前にプログラムを停止してください"));
        } else if (ch == 'B')
            snprintf(
                app->g.message, sizeof(app->g.message), "%s",
                tui_text(
                    "Conditional breakpoints require a source location; use F for a function",
                    "条件付きBreakpointにはソース位置が必要です。関数にはFを使用してください"));
        else if ((ch == '\n' || ch == KEY_ENTER) && app->g.instruction_count) {
            GdbInstruction *ins = &app->g.instructions[app->asm_selected];
            if (ins->call && ins->call_target[0]) {
                char target[256];
                snprintf(target, sizeof(target), "%s", ins->call_target);
                if (!gdb_disassemble_at(&app->g, target)) {
                    app->asm_selected = 0;
                    app->asm_top = 0;
                    history_push_assembly(&app->history, target, target, app->asm_selected,
                                          app->asm_top);
                    snprintf(app->g.message, sizeof(app->g.message),
                             tui_text("Opened call target %s", "呼出し先を開きました: %s"), target);
                } else
                    snprintf(
                        app->g.message, sizeof(app->g.message),
                        tui_text("Call target unavailable: %s", "呼出し先を表示できません: %s"),
                        target);
            } else if (ins->call)
                snprintf(app->g.message, sizeof(app->g.message), "%s",
                         tui_text("Indirect call target is unavailable",
                                  "間接呼出しの対象を取得できません"));
            else
                snprintf(app->g.message, sizeof(app->g.message),
                         tui_text("Selected instruction: %s", "選択中の命令: %s"), ins->address);
        } else if (ch == 'r')
            gdb_run(&app->g);
        else if (ch == 'c' && stopped)
            gdb_continue(&app->g);
        else if (ch == 'f' && stopped)
            gdb_finish(&app->g);
        else if (ch == 'S') {
            gdb_refresh_breakpoints(&app->g);
            app->view = VIEW_BREAKS;
        } else if (ch == 'O')
            app->view = VIEW_OUTPUT;
        else if (ch == 'h')
            app->view = VIEW_HELP;
        else if (ch == 'q') {
            if (app->g.state == GDB_RUNNING || app->g.state == GDB_STOPPED) {
                if (confirm_quit(&app->g))
                    app->done = true;
            } else
                app->done = true;
        } else
            snprintf(
                app->g.message, sizeof(app->g.message), "%s",
                tui_text("ASM: j/k browse, i/I step, b address breakpoint, B function breakpoint",
                         "ASM: j/k 移動、i/I 命令実行、b アドレスBreakpoint、F 関数Breakpoint"));
        return true;
    }
    if (app->mode == MODE_VIM) {
        if (isdigit(ch) && (ch != '0' || app->vim_count)) {
            app->vim_count = app->vim_count * 10 + (ch - '0');
            if (app->vim_count > 9999)
                app->vim_count = 9999;
            snprintf(app->g.message, sizeof(app->g.message), tui_text("count: %d", "回数: %d"),
                     app->vim_count);
            return true;
        }
        int count = app->vim_count ? app->vim_count : 1;
        app->vim_count = 0;
        if (ch == 'h' || ch == KEY_LEFT) {
            app->source_col -= count;
            clamp_column(&app->src, app->cursor, &app->source_col);
        } else if (ch == 'l' || ch == KEY_RIGHT) {
            app->source_col += count;
            clamp_column(&app->src, app->cursor, &app->source_col);
        } else if (ch == 'j' || ch == KEY_DOWN) {
            app->cursor += count;
            if (app->cursor >= app->src.count)
                app->cursor = app->src.count ? app->src.count - 1 : 0;
            clamp_column(&app->src, app->cursor, &app->source_col);
        } else if (ch == 'k' || ch == KEY_UP) {
            app->cursor -= count;
            if (app->cursor < 0)
                app->cursor = 0;
            clamp_column(&app->src, app->cursor, &app->source_col);
        } else if (ch == 'w') {
            for (int i = 0; i < count; i++)
                word_motion(&app->src, &app->cursor, &app->source_col, 1, false);
        } else if (ch == 'b') {
            for (int i = 0; i < count; i++)
                word_motion(&app->src, &app->cursor, &app->source_col, -1, false);
        } else if (ch == 'e') {
            for (int i = 0; i < count; i++)
                word_motion(&app->src, &app->cursor, &app->source_col, 1, true);
        } else if (ch == '0')
            app->source_col = 0;
        else if (ch == '^') {
            app->source_col = 0;
            if (app->src.count)
                while (app->src.lines[app->cursor][app->source_col] &&
                       isspace((unsigned char)app->src.lines[app->cursor][app->source_col]))
                    app->source_col++;
            clamp_column(&app->src, app->cursor, &app->source_col);
        } else if (ch == '$') {
            app->source_col = app->src.count ? (int)strlen(app->src.lines[app->cursor]) - 1 : 0;
            clamp_column(&app->src, app->cursor, &app->source_col);
        } else if (ch == 'G') {
            app->cursor = app->src.count ? app->src.count - 1 : 0;
            app->source_col = 0;
        } else if (ch == 'H') {
            app->cursor = app->top;
            clamp_column(&app->src, app->cursor, &app->source_col);
        } else if (ch == 'M') {
            app->cursor = app->top + app->source_h / 2;
            if (app->cursor >= app->src.count)
                app->cursor = app->src.count - 1;
            clamp_column(&app->src, app->cursor, &app->source_col);
        } else if (ch == 'L') {
            app->cursor = app->top + app->source_h - 1;
            if (app->cursor >= app->src.count)
                app->cursor = app->src.count - 1;
            clamp_column(&app->src, app->cursor, &app->source_col);
        } else if (ch == KEY_NPAGE || ch == 6) {
            app->cursor += count * (app->source_h > 1 ? app->source_h - 1 : 1);
            if (app->cursor >= app->src.count)
                app->cursor = app->src.count - 1;
            clamp_column(&app->src, app->cursor, &app->source_col);
        } else if (ch == KEY_PPAGE || ch == 2) {
            app->cursor -= count * (app->source_h > 1 ? app->source_h - 1 : 1);
            if (app->cursor < 0)
                app->cursor = 0;
            clamp_column(&app->src, app->cursor, &app->source_col);
        } else if (ch == 4) {
            app->cursor += count * (app->source_h / 2 > 0 ? app->source_h / 2 : 1);
            if (app->cursor >= app->src.count)
                app->cursor = app->src.count - 1;
            clamp_column(&app->src, app->cursor, &app->source_col);
        } else if (ch == 21) {
            app->cursor -= count * (app->source_h / 2 > 0 ? app->source_h / 2 : 1);
            if (app->cursor < 0)
                app->cursor = 0;
            clamp_column(&app->src, app->cursor, &app->source_col);
        } else if (ch == '{' || ch == '}') {
            for (int i = 0; i < count; i++)
                paragraph_motion(&app->src, &app->cursor, ch == '}' ? 1 : -1);
            app->source_col = 0;
        } else if (ch == '%') {
            if (!match_bracket(&app->src, &app->cursor, &app->source_col))
                snprintf(app->g.message, sizeof(app->g.message), "%s",
                         tui_text("No matching bracket", "対応する括弧がありません"));
        } else if (ch == 'g') {
            timeout(-1);
            int next = getch();
            timeout(80);
            if (next == 'g') {
                app->cursor = 0;
                app->source_col = 0;
            } else if (next == 'd' || next == 'D') {
                char word[256] = "";
                word_under_cursor(&app->src, app->cursor, app->source_col, word, sizeof(word),
                                  NULL);
                if (word[0]) {
                    snprintf(app->last_search, sizeof(app->last_search), "%s", word);
                    app->search_direction = 1;
                    app->search_whole_word = true;
                }
                if (goto_declaration(&app->src, &app->cursor, &app->source_col, next == 'D'))
                    snprintf(app->g.message, sizeof(app->g.message),
                             tui_text("%s -> %s at line %d; n/N repeat",
                                      "%s -> %s は%d行目。n/Nで繰返し"),
                             next == 'D' ? "gD" : "gd", word, app->cursor + 1);
                else
                    snprintf(app->g.message, sizeof(app->g.message),
                             tui_text("Declaration not found: %s", "宣言が見つかりません: %s"),
                             word[0] ? word : tui_text("<no identifier>", "<識別子なし>"));
            } else
                snprintf(app->g.message, sizeof(app->g.message), "%s",
                         tui_text("Unknown g command", "不明なgコマンドです"));
        } else if (ch == 'z') {
            timeout(-1);
            int next = getch();
            timeout(80);
            if (next == 'z')
                app->top = app->cursor - app->source_h / 2;
            else if (next == 't')
                app->top = app->cursor;
            else if (next == 'b')
                app->top = app->cursor - app->source_h + 1;
            else
                snprintf(app->g.message, sizeof(app->g.message), "%s",
                         tui_text("Unknown z command", "不明なzコマンドです"));
            if (app->top < 0)
                app->top = 0;
        } else if (ch == '/' || ch == '?') {
            char input[1024] = "";
            if (prompt_text(ch == '/' ? tui_text("Search forward: ", "前方検索: ")
                                      : tui_text("Search backward: ", "後方検索: "),
                            input, sizeof(input))) {
                snprintf(app->last_search, sizeof(app->last_search), "%s", input);
                app->search_direction = ch == '/' ? 1 : -1;
                app->search_whole_word = false;
                int found_line = app->cursor, found_col = app->source_col;
                if (text_search_position(&app->src, &found_line, &found_col, app->last_search,
                                         app->search_direction, app->search_whole_word)) {
                    app->cursor = found_line;
                    app->source_col = found_col;
                    snprintf(app->g.message, sizeof(app->g.message),
                             tui_text("Search '%.*s': line %d", "検索 '%.*s': %d行目"), 700,
                             app->last_search, app->cursor + 1);
                } else
                    snprintf(
                        app->g.message, sizeof(app->g.message),
                        tui_text("Search text not found: %.*s", "検索文字列が見つかりません: %.*s"),
                        700, app->last_search);
            }
        } else if (ch == 'n' || ch == 'N') {
            if (app->last_search[0]) {
                int direction = ch == 'n' ? app->search_direction : -app->search_direction,
                    found_line = app->cursor, found_col = app->source_col;
                if (text_search_position(&app->src, &found_line, &found_col, app->last_search,
                                         direction, app->search_whole_word)) {
                    app->cursor = found_line;
                    app->source_col = found_col;
                    snprintf(app->g.message, sizeof(app->g.message),
                             tui_text("Search '%.*s': line %d", "検索 '%.*s': %d行目"), 700,
                             app->last_search, app->cursor + 1);
                } else
                    snprintf(
                        app->g.message, sizeof(app->g.message),
                        tui_text("Search text not found: %.*s", "検索文字列が見つかりません: %.*s"),
                        700, app->last_search);
            } else
                snprintf(app->g.message, sizeof(app->g.message), "%s",
                         tui_text("No previous search", "直前の検索がありません"));
        } else if (ch == '*' || ch == '#') {
            char word[256];
            if (word_under_cursor(&app->src, app->cursor, app->source_col, word, sizeof(word),
                                  NULL)) {
                snprintf(app->last_search, sizeof(app->last_search), "%s", word);
                app->search_direction = ch == '*' ? 1 : -1;
                app->search_whole_word = true;
                if (search_word(&app->src, &app->cursor, &app->source_col, app->last_search,
                                app->search_direction))
                    snprintf(app->g.message, sizeof(app->g.message),
                             tui_text("Search word '%s': line %d", "単語検索 '%s': %d行目"),
                             app->last_search, app->cursor + 1);
                else
                    snprintf(app->g.message, sizeof(app->g.message),
                             tui_text("Word not found: %s", "単語が見つかりません: %s"),
                             app->last_search);
            }
        } else if (ch == 'q') {
            if (app->g.state == GDB_RUNNING || app->g.state == GDB_STOPPED) {
                if (confirm_quit(&app->g))
                    app->done = true;
            } else
                app->done = true;
        } else
            snprintf(app->g.message, sizeof(app->g.message), "%s",
                     tui_text("VIM mode: Tab switches to GDB controls",
                              "VIMモード: TabでGDB操作へ切り替え"));
        return true;
    }
    return false;
}
