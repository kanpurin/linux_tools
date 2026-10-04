#include "app_state.h"
#include "ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool app_handle_view(App *app, int ch) {
    if (app->view == VIEW_EDIT_VALUE) {
        if (app->edit_dialog.confirm) {
            if (ch == 'y' || ch == 'Y') {
                char actual[GD_TEXT_MAX] = "", before[GD_TEXT_MAX], target[512];
                snprintf(before, sizeof(before), "%s", app->edit_dialog.current);
                snprintf(target, sizeof(target), "%s", app->edit_dialog.expression);
                int rc =
                    app->edit_dialog.kind == EDIT_REGISTER
                        ? gdb_assign_register(&app->g, app->edit_dialog.label,
                                              app->edit_dialog.input, actual, sizeof(actual))
                        : gdb_assign_expression(&app->g, app->edit_dialog.expression,
                                                app->edit_dialog.input, actual, sizeof(actual));
                app->view = VIEW_MAIN;
                if (!rc) {
                    if (app->edit_dialog.kind == EDIT_VARIABLE)
                        refresh_expansions(&app->g, app->expansions, &app->expansion_count);
                    snprintf(app->g.message, sizeof(app->g.message),
                             tui_text("Modified: %.300s  %.300s -> %.300s",
                                      "変更しました: %.300s  %.300s -> %.300s"),
                             target, before, actual);
                }
            } else {
                app->view = VIEW_MAIN;
                snprintf(app->g.message, sizeof(app->g.message), "%s",
                         tui_text("Value modification cancelled", "値の変更を中止しました"));
            }
        } else if (ch == 27) {
            app->view = VIEW_MAIN;
            snprintf(app->g.message, sizeof(app->g.message), "%s",
                     tui_text("Value modification cancelled", "値の変更を中止しました"));
        } else if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) {
            size_t len = strlen(app->edit_dialog.input);
            if (len)
                app->edit_dialog.input[len - 1] = '\0';
        } else if (ch == '\n' || ch == KEY_ENTER) {
            if (app->edit_dialog.input[0])
                app->edit_dialog.confirm = true;
            else
                snprintf(app->g.message, sizeof(app->g.message), "%s",
                         tui_text("New value is required", "新しい値を入力してください"));
        } else if (ch >= 32 && ch < 127) {
            size_t len = strlen(app->edit_dialog.input);
            if (len + 1 < sizeof(app->edit_dialog.input)) {
                app->edit_dialog.input[len] = (char)ch;
                app->edit_dialog.input[len + 1] = '\0';
            }
        }
        return true;
    }
    if (app->view == VIEW_FORCE_RETURN) {
        if (app->force_return_dialog.confirm) {
            if (ch == 'y' || ch == 'Y') {
                char function[256], value[GD_TEXT_MAX];
                snprintf(function, sizeof(function), "%s", app->force_return_dialog.function);
                snprintf(value, sizeof(value), "%s", app->force_return_dialog.input);
                int rc = gdb_force_return(&app->g, app->force_return_dialog.is_void ? NULL : value);
                bool raw_result = !rc && (strstr(app->g.message, "raw System V") != NULL ||
                                          strstr(app->g.message, "System V整数") != NULL);
                app->view = VIEW_MAIN;
                if (!rc) {
                    app->expansion_count = 0;
                    app->var_selected = 0;
                    app->var_top = 0;
                    app->stack_selected = 0;
                    app->asm_selected =
                        app->g.current_instruction >= 0 ? app->g.current_instruction : 0;
                    app->asm_top = 0;
                    app->focus = PANE_SOURCE;
                    if (app->g.source_available &&
                        source_navigate(&app->src, &app->history, app->g.fullname, app->g.line,
                                        &app->cursor, &app->source_col, &app->top, &app->hscroll,
                                        true, true))
                        app->code_view = CODE_SOURCE;
                    else {
                        app->code_view = CODE_ASSEMBLY;
                        if (app->g.pc[0])
                            history_push_assembly(&app->history, app->g.pc, app->g.function,
                                                  app->asm_selected, app->asm_top);
                    }
                    snprintf(app->g.message, sizeof(app->g.message),
                             tui_text("Forced return: %.250s()%s%.650s%s",
                                      "強制returnしました: %.250s()%s%.650s%s"),
                             function, value[0] ? " -> " : "", value,
                             raw_result ? " [raw rax]" : "");
                }
            } else {
                app->view = VIEW_MAIN;
                snprintf(app->g.message, sizeof(app->g.message), "%s",
                         tui_text("Force return cancelled", "強制returnを中止しました"));
            }
        } else if (ch == 27) {
            app->view = VIEW_MAIN;
            snprintf(app->g.message, sizeof(app->g.message), "%s",
                     tui_text("Force return cancelled", "強制returnを中止しました"));
        } else if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) {
            size_t len = strlen(app->force_return_dialog.input);
            if (len)
                app->force_return_dialog.input[len - 1] = '\0';
        } else if (ch == '\n' || ch == KEY_ENTER) {
            if (!app->force_return_dialog.is_void && app->force_return_dialog.type_known &&
                !app->force_return_dialog.input[0])
                snprintf(app->g.message, sizeof(app->g.message),
                         tui_text("Return value is required for %s", "%sの戻り値が必要です"),
                         app->force_return_dialog.return_type);
            else
                app->force_return_dialog.confirm = true;
        } else if (ch >= 32 && ch < 127 && !app->force_return_dialog.is_void) {
            size_t len = strlen(app->force_return_dialog.input);
            if (len + 1 < sizeof(app->force_return_dialog.input)) {
                app->force_return_dialog.input[len] = (char)ch;
                app->force_return_dialog.input[len + 1] = '\0';
            }
        }
        return true;
    }
    if (app->view == VIEW_CATCH_MENU) {
        const char *events[] = {"syscall", "signal", "fork", "vfork", "exec", "load"};
        if (ch == 27) {
            app->view = VIEW_MAIN;
            snprintf(app->g.message, sizeof(app->g.message), "%s",
                     tui_text("Catchpoint cancelled", "Catchpointを中止しました"));
        } else if ((ch == 'j' || ch == KEY_DOWN) && app->catch_selected < 5)
            app->catch_selected++;
        else if ((ch == 'k' || ch == KEY_UP) && app->catch_selected > 0)
            app->catch_selected--;
        else if (ch == '\n' || ch == KEY_ENTER) {
            const char *event = events[app->catch_selected];
            if (app->catch_selected < 2) {
                memset(&app->catch_dialog, 0, sizeof(app->catch_dialog));
                snprintf(app->catch_dialog.event, sizeof(app->catch_dialog.event), "%s", event);
                refresh_catch_dialog(&app->g, &app->catch_dialog);
                app->view = VIEW_CATCH_SEARCH;
            } else {
                if (!gdb_set_catchpoint(&app->g, event, NULL))
                    app->view = VIEW_MAIN;
            }
        }
        return true;
    }
    if (app->view == VIEW_CATCH_SEARCH) {
        if (ch == 27) {
            app->view = VIEW_MAIN;
            snprintf(app->g.message, sizeof(app->g.message), "%s",
                     tui_text("Catchpoint cancelled", "Catchpointを中止しました"));
        } else if ((ch == 'j' || ch == KEY_DOWN) &&
                   app->catch_dialog.selected + 1 < app->catch_dialog.count)
            app->catch_dialog.selected++;
        else if ((ch == 'k' || ch == KEY_UP) && app->catch_dialog.selected > 0)
            app->catch_dialog.selected--;
        else if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) {
            size_t len = strlen(app->catch_dialog.input);
            if (len)
                app->catch_dialog.input[len - 1] = '\0';
            refresh_catch_dialog(&app->g, &app->catch_dialog);
        } else if (ch == '\n' || ch == KEY_ENTER) {
            char target[128] = "";
            if (app->catch_dialog.count)
                snprintf(target, sizeof(target), "%s",
                         app->catch_dialog.candidates[app->catch_dialog.selected]);
            else
                snprintf(target, sizeof(target), "%s", app->catch_dialog.input);
            if (!target[0])
                snprintf(app->g.message, sizeof(app->g.message),
                         tui_text("Select or enter a %s name", "%s名を選択または入力してください"),
                         app->catch_dialog.event);
            else if (!gdb_set_catchpoint(&app->g, app->catch_dialog.event, target))
                app->view = VIEW_MAIN;
        } else if (ch >= 32 && ch < 127) {
            size_t len = strlen(app->catch_dialog.input);
            if (len + 1 < sizeof(app->catch_dialog.input)) {
                app->catch_dialog.input[len] = (char)ch;
                app->catch_dialog.input[len + 1] = '\0';
                refresh_catch_dialog(&app->g, &app->catch_dialog);
            }
        }
        return true;
    }
    if (app->view == VIEW_OPEN_MENU) {
        if (ch == 27) {
            app->view = VIEW_MAIN;
            snprintf(app->g.message, sizeof(app->g.message), "%s",
                     tui_text("Open cancelled", "開く操作を中止しました"));
        } else if ((ch == 'j' || ch == KEY_DOWN || ch == 'k' || ch == KEY_UP))
            app->open_selected = 1 - app->open_selected;
        else if (ch == '\n' || ch == KEY_ENTER) {
            if (app->open_selected == 0) {
                memset(&app->function_dialog, 0, sizeof(app->function_dialog));
                app->function_dialog.action = FUNCTION_OPEN;
                refresh_function_dialog(&app->g, &app->function_dialog);
                app->view = VIEW_FUNCTIONS;
            } else {
                load_file_dialog(&app->g, app->project_root, &app->file_dialog);
                app->view = VIEW_OPEN_FILES;
            }
        }
        return true;
    }
    if (app->view == VIEW_OPEN_FILES) {
        if (ch == 27) {
            app->view = VIEW_MAIN;
            snprintf(app->g.message, sizeof(app->g.message), "%s",
                     tui_text("Open file cancelled", "ファイルを開く操作を中止しました"));
        } else if ((ch == 'j' || ch == KEY_DOWN) &&
                   app->file_dialog.selected + 1 < app->file_dialog.count)
            app->file_dialog.selected++;
        else if ((ch == 'k' || ch == KEY_UP) && app->file_dialog.selected > 0)
            app->file_dialog.selected--;
        else if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) {
            size_t len = strlen(app->file_dialog.input);
            if (len)
                app->file_dialog.input[len - 1] = '\0';
            filter_file_dialog(&app->file_dialog);
        } else if ((ch == '\n' || ch == KEY_ENTER) && app->file_dialog.count) {
            SourceFileCandidate *f =
                &app->file_dialog.all[app->file_dialog.matches[app->file_dialog.selected]];
            if (source_navigate(&app->src, &app->history, f->path, 1, &app->cursor,
                                &app->source_col, &app->top, &app->hscroll, f->debug_lines, true)) {
                app->focus = PANE_SOURCE;
                app->code_view = CODE_SOURCE;
                app->view = VIEW_MAIN;
                snprintf(app->g.message, sizeof(app->g.message),
                         tui_text("Opened %s%s", "開きました: %s%s"), f->display,
                         f->debug_lines ? "" : tui_text(" [REFERENCE ONLY]", " [参照のみ]"));
            } else
                snprintf(app->g.message, sizeof(app->g.message),
                         tui_text("ERROR: source unavailable: %.900s",
                                  "エラー: ソースを表示できません: %.900s"),
                         f->path);
        } else if (ch >= 32 && ch < 127) {
            size_t len = strlen(app->file_dialog.input);
            if (len + 1 < sizeof(app->file_dialog.input)) {
                app->file_dialog.input[len] = (char)ch;
                app->file_dialog.input[len + 1] = '\0';
                filter_file_dialog(&app->file_dialog);
            }
        }
        return true;
    }
    if (app->view == VIEW_FUNCTIONS) {
        if (ch == 27) {
            app->view = VIEW_MAIN;
            snprintf(
                app->g.message, sizeof(app->g.message), "%s",
                app->function_dialog.action == FUNCTION_OPEN
                    ? tui_text("Open function cancelled", "関数を開く操作を中止しました")
                    : tui_text("Function breakpoint cancelled", "関数Breakpointを中止しました"));
        } else if ((ch == 'j' || ch == KEY_DOWN) &&
                   app->function_dialog.selected + 1 < app->function_dialog.count)
            app->function_dialog.selected++;
        else if ((ch == 'k' || ch == KEY_UP) && app->function_dialog.selected > 0)
            app->function_dialog.selected--;
        else if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) {
            size_t len = strlen(app->function_dialog.input);
            if (len)
                app->function_dialog.input[len - 1] = '\0';
            refresh_function_dialog(&app->g, &app->function_dialog);
        } else if (ch == '\n' || ch == KEY_ENTER) {
            char target[512] = "";
            int chosen = -1;
            for (int i = 0; i < app->function_dialog.count; i++) {
                if (app->function_dialog.input[0] &&
                    !strcmp(app->function_dialog.input, app->function_dialog.candidates[i].name)) {
                    chosen = i;
                    snprintf(target, sizeof(target), "%s", app->function_dialog.input);
                    break;
                }
            }
            if (chosen < 0 && app->function_dialog.count) {
                chosen = app->function_dialog.selected;
                snprintf(target, sizeof(target), "%s",
                         app->function_dialog.candidates[chosen].name);
            }
            if (!target[0] && app->function_dialog.input[0])
                snprintf(target, sizeof(target), "%s", app->function_dialog.input);
            if (!target[0])
                snprintf(app->g.message, sizeof(app->g.message), "%s",
                         tui_text("No function selected", "関数を選択してください"));
            else if (app->function_dialog.action == FUNCTION_BREAKPOINT) {
                if (gdb_set_function_breakpoint(&app->g, target))
                    snprintf(app->g.message, sizeof(app->g.message),
                             tui_text("Function not found: %.900s", "関数が見つかりません: %.900s"),
                             target);
                else
                    app->view = VIEW_MAIN;
            } else if (chosen < 0)
                snprintf(app->g.message, sizeof(app->g.message),
                         tui_text("Function not found: %.900s", "関数が見つかりません: %.900s"),
                         target);
            else {
                GdbFunction *f = &app->function_dialog.candidates[chosen];
                if (f->source[0] && f->line > 0 &&
                    source_navigate(&app->src, &app->history, f->source, f->line, &app->cursor,
                                    &app->source_col, &app->top, &app->hscroll, true, true)) {
                    app->focus = PANE_SOURCE;
                    app->code_view = CODE_SOURCE;
                    app->view = VIEW_MAIN;
                    snprintf(app->g.message, sizeof(app->g.message),
                             tui_text("Opened function %s at %s:%d", "関数%sを開きました: %s:%d"),
                             f->name, path_name(f->source), f->line);
                } else if (f->address[0] && !gdb_disassemble_at(&app->g, f->address)) {
                    app->focus = PANE_SOURCE;
                    app->code_view = CODE_ASSEMBLY;
                    app->asm_selected =
                        app->g.current_instruction >= 0 ? app->g.current_instruction : 0;
                    app->asm_top = 0;
                    history_push_assembly(&app->history, f->address, f->name, app->asm_selected,
                                          app->asm_top);
                    app->view = VIEW_MAIN;
                    snprintf(app->g.message, sizeof(app->g.message),
                             tui_text("Opened function %s in Disassembly",
                                      "関数%sを逆アセンブルで開きました"),
                             f->name);
                } else
                    snprintf(app->g.message, sizeof(app->g.message),
                             tui_text("Source and disassembly unavailable: %.800s",
                                      "ソースと逆アセンブルを表示できません: %.800s"),
                             f->name);
            }
        } else if (ch >= 32 && ch < 127) {
            size_t len = strlen(app->function_dialog.input);
            if (len + 1 < sizeof(app->function_dialog.input)) {
                app->function_dialog.input[len] = (char)ch;
                app->function_dialog.input[len + 1] = '\0';
                /* Coalesce already queued text, but preserve selection keys,
                   Enter, Escape and Backspace as separate ordered actions. */
                timeout(0);
                for (;;) {
                    int next = getch();
                    if (next == ERR)
                        break;
                    len = strlen(app->function_dialog.input);
                    if (next >= 32 && next < 127 && next != 'j' && next != 'k' &&
                        len + 1 < sizeof(app->function_dialog.input)) {
                        app->function_dialog.input[len] = (char)next;
                        app->function_dialog.input[len + 1] = '\0';
                    } else {
                        ungetch(next);
                        break;
                    }
                }
                timeout(80);
                refresh_function_dialog(&app->g, &app->function_dialog);
            }
        }
        return true;
    }
    if (app->view == VIEW_HELP) {
        if (ch == 27 || ch == '?' || ch == 'h' || ch == 'q')
            app->view = VIEW_MAIN;
        return true;
    }
    if (app->view != VIEW_MAIN) {
        if (ch == 27 || (app->view == VIEW_OUTPUT && ch == 'O') ||
            (app->view == VIEW_BREAKS && ch == 'S') ||
            (app->view == VIEW_ADDRESS && (ch == 'a' || ch == '\n' || ch == KEY_ENTER)) ||
            (app->view == VIEW_REGISTER && (ch == 'p' || ch == '\n' || ch == KEY_ENTER))) {
            app->view = VIEW_MAIN;
            return true;
        }
        if (app->view == VIEW_BREAKS && app->g.break_count) {
            if (ch == 'j' || ch == KEY_DOWN) {
                if (app->bsel < app->g.break_count - 1)
                    app->bsel++;
            } else if (ch == 'k' || ch == KEY_UP) {
                if (app->bsel > 0)
                    app->bsel--;
            } else if (ch == 'd') {
                gdb_delete_breakpoint(&app->g, app->g.breaks[app->bsel].number);
                if (app->bsel >= app->g.break_count)
                    app->bsel = app->g.break_count - 1;
                if (app->bsel < 0)
                    app->bsel = 0;
            } else if (ch == 'e')
                gdb_enable_breakpoint(&app->g, app->g.breaks[app->bsel].number,
                                      !app->g.breaks[app->bsel].enabled);
            else if (ch == '\n' || ch == KEY_ENTER) {
                GdbBreakpoint *b = &app->g.breaks[app->bsel];
                if (b->catchpoint)
                    snprintf(app->g.message, sizeof(app->g.message),
                             tui_text("Catchpoint C#%d stops on %s%s%s",
                                      "Catchpoint C#%d の停止対象: %s%s%s"),
                             b->number, b->catch_type, b->catch_target[0] ? " " : "",
                             b->catch_target);
                else if (b->watchpoint)
                    snprintf(app->g.message, sizeof(app->g.message), "%s",
                             tui_text("Watchpoints have no browse location",
                                      "Watchpointには移動先がありません"));
                else if (b->file[0] && b->line > 0 &&
                         source_navigate(&app->src, &app->history, b->file, b->line, &app->cursor,
                                         &app->source_col, &app->top, &app->hscroll, true, true)) {
                    app->focus = PANE_SOURCE;
                    app->code_view = CODE_SOURCE;
                    app->view = VIEW_MAIN;
                    snprintf(app->g.message, sizeof(app->g.message),
                             tui_text("Breakpoint B#%d: %s:%d", "Breakpoint B#%d: %s:%d"),
                             b->number, path_name(b->file), b->line);
                } else if (b->function_breakpoint && !b->pending &&
                           !gdb_disassemble_at(&app->g, b->address)) {
                    app->focus = PANE_SOURCE;
                    app->code_view = CODE_ASSEMBLY;
                    app->asm_selected = 0;
                    app->asm_top = 0;
                    history_push_assembly(&app->history, b->address, b->function, app->asm_selected,
                                          app->asm_top);
                    app->view = VIEW_MAIN;
                    snprintf(app->g.message, sizeof(app->g.message),
                             tui_text("Function %s @ %s", "関数 %s @ %s"),
                             b->function[0] ? b->function : b->original_location, b->address);
                } else
                    snprintf(app->g.message, sizeof(app->g.message), "%s",
                             b->pending ? tui_text("Function breakpoint is pending",
                                                   "関数Breakpointは保留中です")
                                        : tui_text("This stop condition has no browse location",
                                                   "この停止条件には移動先がありません"));
            }
        }
        return true;
    }
    return false;
}
