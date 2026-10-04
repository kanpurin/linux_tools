#include "render.h"
#include "source.h"
#include "stops.h"
#include "ui_common.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void draw_stop_summary(Gdb *g, int cols) {
    int breaks = 0, conditional = 0, catches = 0, watches = 0, disabled = 0;
    for (int i = 0; i < g->break_count; i++) {
        GdbBreakpoint *b = &g->breaks[i];
        if (!b->enabled)
            disabled++;
        if (b->watchpoint)
            watches++;
        else if (b->catchpoint)
            catches++;
        else {
            breaks++;
            if (b->condition[0])
                conditional++;
        }
    }
    move(1, 0);
    clrtoeol();
    attron(A_BOLD);
    addstr(tui_text(" STOPS ", " 停止条件 "));
    attroff(A_BOLD);
    attron(COLOR_PAIR(COLOR_BREAK));
    printw("[B] %d", breaks);
    attroff(COLOR_PAIR(COLOR_BREAK));
    if (conditional)
        printw(tui_text(" (cond %d)", " (条件 %d)"), conditional);
    attron(COLOR_PAIR(COLOR_COND));
    printw("  [C] %d", catches);
    attroff(COLOR_PAIR(COLOR_COND));
    attron(COLOR_PAIR(COLOR_WATCH));
    printw("  [W] %d", watches);
    attroff(COLOR_PAIR(COLOR_WATCH));
    if (disabled) {
        attron(COLOR_PAIR(COLOR_MUTED) | A_DIM);
        printw(tui_text("  off %d", "  無効 %d"), disabled);
        attroff(COLOR_PAIR(COLOR_MUTED) | A_DIM);
    }
    if (g->stop_breakpoint) {
        GdbBreakpoint *b = breakpoint_by_number(g, g->stop_breakpoint);
        int x = getcurx(stdscr);
        if (x < cols - 12) {
            addstr(tui_text("  | last: ", "  | 最終: "));
            if (b) {
                attron(COLOR_PAIR(break_color(b)) | A_BOLD);
                printw("%c#%d", b->watchpoint ? 'W' : b->catchpoint ? 'C' : 'B', b->number);
                attroff(COLOR_PAIR(break_color(b)) | A_BOLD);
                if (b->watchpoint && b->condition[0])
                    printw(" %s", b->condition);
            } else
                printw("#%d", g->stop_breakpoint);
            if (g->watch_old[0] || g->watch_new[0])
                printw(" %s -> %s", g->watch_old[0] ? g->watch_old : "?",
                       g->watch_new[0] ? g->watch_new : "?");
        }
    }
    if (g->catch_event[0] && getcurx(stdscr) < cols - 18) {
        attron(COLOR_PAIR(COLOR_COND) | A_BOLD);
        printw("  | %s%s%s", g->catch_event, g->catch_target[0] ? ":" : "", g->catch_target);
        if (g->catch_phase[0])
            printw(" %s", g->catch_phase);
        attroff(COLOR_PAIR(COLOR_COND) | A_BOLD);
    }
}

static void draw_source_code(Gdb *g, Source *s, int cursor, int column, int top, int hscroll,
                             InputMode mode, PaneFocus focus, const char *search_text,
                             bool search_whole_word, const char *project_root, int history_index,
                             int history_count, int height, int cols) {
    bool at_exec = g->source_available && s->debug_lines && cursor + 1 == g->line &&
                   same_source_path(s->path, g->fullname);
    char title[384];
    snprintf(title, sizeof(title),
             tui_text("Source [%s]%s%s  [%s] %.210s:%d  history %d/%d",
                      "ソース [%s]%s%s  [%s] %.210s:%d  履歴 %d/%d"),
             at_exec ? tui_text("EXEC", "実行") : tui_text("VIEW", "閲覧"),
             focus == PANE_SOURCE ? " *" : "",
             s->debug_lines ? "" : tui_text(" [REFERENCE ONLY]", " [参照のみ]"),
             project_path(s->path, project_root) ? tui_text("project", "プロジェクト")
                                                 : tui_text("external", "外部"),
             s->path[0] ? path_name(s->path) : "-", cursor + 1,
             history_count ? history_index + 1 : 0, history_count);
    line_rule(2, title);
    if (!s->count) {
        attron(COLOR_PAIR(COLOR_MUTED) | A_DIM);
        clipped(
            4, 2, cols - 4, "%s",
            tui_text("Source file is not available. Press d for Disassembly.",
                     "ソースファイルを表示できません。dで逆アセンブル表示へ切り替えられます。"));
        attroff(COLOR_PAIR(COLOR_MUTED) | A_DIM);
        return;
    }
    for (int r = 0; r < height; r++) {
        int ln = top + r + 1;
        if (ln > s->count)
            break;
        bool current =
            g->source_available && ln == g->line && same_source_path(s->path, g->fullname);
        GdbBreakpoint *bp = breakpoint_at(g, s->path, ln);
        int y = 3 + r, len = (int)strlen(s->lines[ln - 1]), start = hscroll < len ? hscroll : len;
        char marker[16] = "";
        if (bp)
            snprintf(marker, sizeof(marker), "[%c#%d]", bp->enabled ? 'B' : 'd', bp->number);
        if (current) {
            attron(COLOR_PAIR(COLOR_WATCH) | A_BOLD);
            mvaddstr(y, 0, "=>");
            attroff(COLOR_PAIR(COLOR_WATCH) | A_BOLD);
        }
        if (bp) {
            int color = break_color(bp);
            attron(COLOR_PAIR(color) | (bp->enabled ? A_BOLD : A_DIM));
            mvaddnstr(y, 2, marker, 7);
            attroff(COLOR_PAIR(color) | (bp->enabled ? A_BOLD : A_DIM));
        }
        mvaddch(y, 9, ln - 1 == cursor ? '>' : ' ');
        clipped(y, 11, 7, "%5d  ", ln);
        if (hscroll > 0)
            mvaddch(y, 17, '<');
        if (current)
            attron(A_BOLD);
        clipped(y, 18, cols - 18, "%s", s->lines[ln - 1] + start);
        if (current)
            attroff(A_BOLD);
        if (mode == MODE_VIM && search_text && search_text[0]) {
            const char *p = s->lines[ln - 1];
            int slen = (int)strlen(search_text);
            while ((p = strstr(p, search_text))) {
                int pos = (int)(p - s->lines[ln - 1]);
                bool boundary = !search_whole_word ||
                                ((pos == 0 || !word_char(s->lines[ln - 1][pos - 1])) &&
                                 (pos + slen == len || !word_char(s->lines[ln - 1][pos + slen])));
                if (boundary) {
                    int visible_start = pos < hscroll ? hscroll : pos, visible_end = pos + slen;
                    if (visible_end > hscroll && visible_start - hscroll < cols - 18) {
                        int cell = 18 + visible_start - hscroll,
                            count = visible_end - visible_start;
                        if (cell + count > cols)
                            count = cols - cell;
                        if (count > 0)
                            mvchgat(y, cell, count, A_REVERSE | A_BOLD, COLOR_COND, NULL);
                    }
                }
                p += slen;
            }
        }
        if (focus == PANE_SOURCE && ln - 1 == cursor) {
            int cell = 18 + column - hscroll;
            if (cell >= 18 && cell < cols)
                mvchgat(y, cell, 1, A_REVERSE | A_BOLD, COLOR_INFO, NULL);
        }
    }
}

static void draw_disassembly(Gdb *g, int selected, int top, PaneFocus focus, int height, int cols) {
    char title[384];
    const char *function = g->disassembly_function[0] ? g->disassembly_function
                           : g->function[0]           ? g->function
                                                      : tui_text("<unknown>", "<不明>");
    snprintf(title, sizeof(title),
             tui_text("Disassembly [%s]%s  Function: %.180s  RIP: %.120s",
                      "逆アセンブル [%s]%s  関数: %.180s  RIP: %.120s"),
             g->current_instruction >= 0 ? tui_text("EXEC", "実行") : tui_text("VIEW", "閲覧"),
             focus == PANE_SOURCE ? " *" : "", function,
             g->pc[0] ? g->pc : tui_text("<unavailable>", "<取得不可>"));
    line_rule(2, title);
    if (!g->instruction_count) {
        attron(COLOR_PAIR(COLOR_MUTED) | A_DIM);
        clipped(4, 2, cols - 4, "%s",
                tui_text("Disassembly unavailable until the program is stopped.",
                         "プログラムが停止するまで逆アセンブルを表示できません。"));
        attroff(COLOR_PAIR(COLOR_MUTED) | A_DIM);
        return;
    }
    for (int r = 0; r < height; r++) {
        int idx = top + r;
        if (idx >= g->instruction_count)
            break;
        GdbInstruction *ins = &g->instructions[idx];
        GdbBreakpoint *bp = breakpoint_at_address(g, ins->address);
        int y = 3 + r;
        bool selected_row = focus == PANE_SOURCE && idx == selected;
        if (selected_row)
            attron(A_REVERSE);
        if (ins->current) {
            attron(COLOR_PAIR(COLOR_WATCH) | A_BOLD);
            mvaddstr(y, 0, "=>");
            attroff(COLOR_PAIR(COLOR_WATCH) | A_BOLD);
        }
        if (bp) {
            attron(COLOR_PAIR(break_color(bp)) | A_BOLD);
            clipped(y, 2, 7, "[B#%d]", bp->number);
            attroff(COLOR_PAIR(break_color(bp)) | A_BOLD);
        } else
            clipped(y, 2, 7, " ");
        clipped(y, 9, 19, "%-18s", ins->address);
        if (ins->call) {
            attron(COLOR_PAIR(COLOR_COND) | A_BOLD);
            clipped(y, 28, cols - 29, "CALL -> %s  |  %s",
                    ins->call_target[0] ? ins->call_target : tui_text("<indirect>", "<間接呼出し>"),
                    ins->instruction);
            attroff(COLOR_PAIR(COLOR_COND) | A_BOLD);
        } else {
            if (ins->current)
                attron(A_BOLD);
            clipped(y, 28, cols - 29, "%s", ins->instruction);
            if (ins->current)
                attroff(A_BOLD);
        }
        if (selected_row)
            attroff(A_REVERSE);
    }
}

static void draw_variables(Gdb *g, VarRow *var_rows, int var_count, int var_top, int var_selected,
                           PaneFocus focus, int y, int height, int cols) {
    line_rule(y - 1, focus == PANE_VARIABLES ? tui_text("Variables *", "変数 *")
                                             : tui_text("Variables", "変数"));
    if (!g->source_available) {
        attron(COLOR_PAIR(COLOR_MUTED) | A_DIM);
        clipped(y + 1, 2, cols - 4, "%s",
                tui_text("Debug information unavailable.", "デバッグ情報を取得できません。"));
        clipped(
            y + 2, 2, cols - 4, "%s",
            tui_text("Use Registers / Disassembly.", "レジスタ／逆アセンブルを使用してください。"));
        attroff(COLOR_PAIR(COLOR_MUTED) | A_DIM);
        return;
    }
    for (int r = 0; r < height; r++) {
        int idx = var_top + r;
        if (idx >= var_count)
            break;
        VarRow *v = &var_rows[idx];
        if (v->header) {
            attron(A_BOLD | COLOR_PAIR(COLOR_INFO));
            clipped(y + r, 2, cols - 3, "%s", v->label);
            attroff(A_BOLD | COLOR_PAIR(COLOR_INFO));
            continue;
        }
        bool selected = focus == PANE_VARIABLES && idx == var_selected;
        if (selected)
            attron(A_REVERSE);
        int x = 3 + v->depth * 2;
        char marker = v->expanded ? '-' : v->expandable ? '+' : ' ';
        mvaddch(y + r, x++, marker);
        mvaddch(y + r, x++, ' ');
        int wid = watchpoint_for_expression(g, v->expression);
        if (wid) {
            attron(COLOR_PAIR(COLOR_WATCH) | A_BOLD);
            char tag[24];
            snprintf(tag, sizeof(tag), "[W#%d] ", wid);
            mvaddstr(y + r, x, tag);
            x += (int)strlen(tag);
            attroff(COLOR_PAIR(COLOR_WATCH) | A_BOLD);
        }
        clipped(y + r, x, cols - x - 1, "%s = %s%s%s", v->label, v->value, v->type[0] ? "  " : "",
                v->type);
        if (selected)
            attroff(A_REVERSE);
    }
    if (var_count == 0) {
        attron(COLOR_PAIR(COLOR_MUTED) | A_DIM);
        clipped(y, 2, cols - 3, "%s",
                tui_text("(variables unavailable; stop the program first)",
                         "（変数を表示するにはプログラムを停止してください）"));
        attroff(COLOR_PAIR(COLOR_MUTED) | A_DIM);
    }
}

static int syscall_argument(const char *name) {
    const char *names[] = {"rdi", "rsi", "rdx", "r10", "r8", "r9"};
    for (int i = 0; i < 6; i++)
        if (!strcmp(name, names[i]))
            return i + 1;
    return 0;
}
static void draw_registers(Gdb *g, int selected, int top, PaneFocus focus, int y, int height,
                           int cols, bool call_selected) {
    bool syscall = !strcmp(g->catch_event, "syscall");
    char title[200];
    snprintf(title, sizeof(title), tui_text("Registers%s%s%s%s%s", "レジスタ%s%s%s%s%s"),
             focus == PANE_REGISTERS ? " *" : "", syscall ? "  [syscall " : "",
             syscall ? (g->catch_phase[0] ? g->catch_phase : "stop") : "", syscall ? "]" : "",
             !syscall && call_selected ? tui_text("  [call arguments]", "  [呼出し引数]") : "");
    line_rule(y - 1, title);
    if (!g->register_count) {
        attron(COLOR_PAIR(COLOR_MUTED) | A_DIM);
        clipped(y + 1, 2, cols - 4, "%s",
                tui_text("Registers unavailable until the program is stopped.",
                         "プログラムが停止するまでレジスタを表示できません。"));
        attroff(COLOR_PAIR(COLOR_MUTED) | A_DIM);
        return;
    }
    if (!g->amd64_sysv) {
        attron(COLOR_PAIR(COLOR_COND));
        clipped(y, 2, cols - 4, "%s",
                tui_text("Argument register mapping unavailable for this architecture.",
                         "このアーキテクチャでは引数レジスタの対応を表示できません。"));
        attroff(COLOR_PAIR(COLOR_COND));
        y++;
        height--;
    }
    for (int row = 0; row < height; row++) {
        int idx = top + row;
        if (idx >= g->register_count)
            break;
        GdbRegister *r = &g->registers[idx];
        int arg = syscall ? syscall_argument(r->name) : r->argument;
        bool sysno = syscall && !strcmp(r->name, "orig_rax");
        bool special = syscall && (!strcmp(r->name, "rax") || sysno || arg);
        bool selected_row = focus == PANE_REGISTERS && idx == selected;
        if (selected_row)
            attron(A_REVERSE);
        if (special || arg)
            attron(COLOR_PAIR(COLOR_INFO) | A_BOLD);
        else if (r->changed)
            attron(COLOR_PAIR(COLOR_WATCH) | A_BOLD);
        if (sysno)
            clipped(y + row, 2, cols - 4, "sysno / %-7s = %s%s%s", r->name,
                    r->changed ? r->previous : r->value, r->changed ? " -> " : "",
                    r->changed ? r->value : "");
        else if (syscall && !strcmp(r->name, "rax") && !strcmp(g->catch_phase, "Return"))
            clipped(y + row, 2, cols - 4,
                    tui_text("return / rax = %s%s%s", "戻り値 / rax = %s%s%s"),
                    r->changed ? r->previous : r->value, r->changed ? " -> " : "",
                    r->changed ? r->value : "");
        else if (arg)
            clipped(y + row, 2, cols - 4, "arg%d / %-7s = %s%s%s", arg, r->name,
                    r->changed ? r->previous : r->value, r->changed ? " -> " : "",
                    r->changed ? r->value : "");
        else
            clipped(y + row, 2, cols - 4, "%-12s = %s%s%s", r->name,
                    r->changed ? r->previous : r->value, r->changed ? " -> " : "",
                    r->changed ? r->value : "");
        if (special || arg)
            attroff(COLOR_PAIR(COLOR_INFO) | A_BOLD);
        else if (r->changed)
            attroff(COLOR_PAIR(COLOR_WATCH) | A_BOLD);
        if (selected_row)
            attroff(A_REVERSE);
    }
}

static void draw_stack(Gdb *g, int selected, PaneFocus focus, int y, int height, int cols,
                       const char *project_root) {
    line_rule(y - 1, focus == PANE_STACK ? tui_text("Stack *  [P] project  [X] external",
                                                    "スタック *  [P] プロジェクト  [X] 外部")
                                         : tui_text("Stack  [P] project  [X] external",
                                                    "スタック  [P] プロジェクト  [X] 外部"));
    for (int i = 0; i < g->frame_count && i < height; i++) {
        GdbFrame *f = &g->frames[i];
        bool selected_row = focus == PANE_STACK && i == selected;
        bool active = f->level == g->selected_frame;
        bool has_source = f->file[0] && f->line > 0;
        bool in_project = has_source && project_path(f->file, project_root);
        if (selected_row)
            attron(A_REVERSE);
        if (active)
            attron(A_BOLD);
        attron(COLOR_PAIR(has_source ? (in_project ? COLOR_INFO : COLOR_COND) : COLOR_MUTED));
        clipped(y + i, 1, cols - 2, "%c #%d %s", active ? '>' : ' ', f->level,
                has_source ? (in_project ? "[P]" : "[X]") : "[A]");
        attroff(COLOR_PAIR(has_source ? (in_project ? COLOR_INFO : COLOR_COND) : COLOR_MUTED));
        const char *func = f->func[0] && strcmp(f->func, "??") ? f->func : NULL;
        if (has_source)
            clipped(y + i, 10, cols - 11, "%s()  %s:%d", func ? func : "?", f->file, f->line);
        else
            clipped(y + i, 10, cols - 11, "%s%s%s", func ? func : "", func ? "()  " : "",
                    f->address[0] ? f->address
                                  : tui_text("<address unavailable>", "<アドレス取得不可>"));
        if (active)
            attroff(A_BOLD);
        if (selected_row)
            attroff(A_REVERSE);
    }
}

void draw_main(Gdb *g, Source *s, int cursor, int column, int top, int hscroll, InputMode mode,
               CodeView code_view, PaneFocus focus, VarRow *var_rows, int var_count, int var_top,
               int var_selected, int reg_selected, int reg_top, int asm_selected, int asm_top,
               int stack_selected, const char *search_text, bool search_whole_word,
               const char *project_root, int history_index, int history_count) {
    int rows, cols, code_h, middle_h, stack_h;
    getmaxyx(stdscr, rows, cols);
    layout_heights(rows, &code_h, &middle_h, &stack_h);
    if (code_view == CODE_ASSEMBLY)
        while (middle_h < 6 && stack_h > 2) {
            middle_h++;
            stack_h--;
        }
    erase();
    attron(A_BOLD | COLOR_PAIR(COLOR_INFO));
    clipped(0, 1, cols - 2, "gd [%s] [%s] [%s]", mode == MODE_VIM ? "VIM" : "GDB",
            code_view == CODE_ASSEMBLY ? "ASM" : "SRC", focus_name(focus));
    attroff(A_BOLD | COLOR_PAIR(COLOR_INFO));
    clipped(0, 30, cols - 31,
            tui_text("| %-11s | Exec frame #%d %s%s%s", "| %-8s | 実行フレーム #%d %s%s%s"),
            state_name(g->state), g->selected_frame, g->function[0] ? g->function : "-",
            g->source_available ? ":" : " @ ", g->source_available ? (char[32]){0} : g->pc);
    if (g->source_available)
        clipped(0, cols > 20 ? cols - 20 : 0, 19, tui_text("Exec line %d", "実行行 %d"), g->line);
    draw_stop_summary(g, cols);
    if (code_view == CODE_ASSEMBLY)
        draw_disassembly(g, asm_selected, asm_top, focus, code_h, cols);
    else
        draw_source_code(g, s, cursor, column, top, hscroll, mode, focus, search_text,
                         search_whole_word, project_root, history_index, history_count, code_h,
                         cols);
    int y = 3 + code_h + 1;
    bool call_selected = asm_selected >= 0 && asm_selected < g->instruction_count &&
                         g->instructions[asm_selected].call;
    if (code_view == CODE_ASSEMBLY)
        draw_registers(g, reg_selected, reg_top, focus, y, middle_h, cols, call_selected);
    else
        draw_variables(g, var_rows, var_count, var_top, var_selected, focus, y, middle_h, cols);
    y += middle_h + 1;
    draw_stack(g, stack_selected, focus, y, stack_h, cols, project_root);
    line_rule(rows - 2, NULL);
    int status_color = !strncmp(g->message, "ERROR", 5) || !strncmp(g->message, "エラー", 9)
                           ? COLOR_ERROR
                       : strstr(g->message, "Watchpoint") || strstr(g->message, "watchpoint") ||
                               strstr(g->message, "ウォッチポイント")
                           ? COLOR_WATCH
                       : strstr(g->message, "Catch") || strstr(g->message, "Caught") ||
                               strstr(g->message, "キャッチポイント")
                           ? COLOR_COND
                           : COLOR_INFO;
    attron(COLOR_PAIR(status_color));
    clipped(rows - 1, 1, cols - 2, "%s", g->message);
    attroff(COLOR_PAIR(status_color));
}
void draw_help_popup(void) {
    static const char *english[] = {"Execution",
                                    "  r / c       Run / Continue",
                                    "  n / s       Next / Step into",
                                    "  f           Finish current function and return to caller",
                                    "              caller has source -> Source, otherwise Assembly",
                                    "  i / I       Step instruction / Next instruction",
                                    "  R           Force return (confirmation required)",
                                    "",
                                    "View / History",
                                    "  Tab         Switch pane    Shift-Tab: reverse",
                                    "  d / e       Source/Assembly / Return to execution position",
                                    "  o           Open Function / File",
                                    "  Ctrl-g      Search project sources",
                                    "  [ / Ctrl-o  View history back    ]: forward",
                                    "              history only changes the view; f executes",
                                    "",
                                    "Stop conditions",
                                    "  b / B / F   Breakpoint / Conditional / Function",
                                    "  C / w / S   Catchpoint / Watchpoint / Stop list",
                                    "",
                                    "Panes",
                                    "  Variables   Enter expand, p value, a address, E edit",
                                    "  Registers   p details, E edit",
                                    "  Stack       Enter selects frame and opens its location",
                                    "",
                                    "Modes / Other",
                                    "  F2          GDB / read-only VIM mode",
                                    "  L           Switch UI language (GDB mode)",
                                    "  VIM         h/j/k/l, /?, n/N, gd/gD, *#, %",
                                    "  O           Program output",
                                    "  ?           Help    q: Quit",
                                    NULL};
    static const char *japanese[] = {
        "実行",
        "  r / c       実行 / 継続",
        "  n / s       次の行 / 関数内へステップ",
        "  f           現在の関数から戻るまで実行",
        "              呼出し元にソースあり -> Source、なし -> Assembly",
        "  i / I       1命令実行 / 次の命令へ",
        "  R           強制return（確認あり）",
        "",
        "表示 / 履歴",
        "  Tab         ペイン切替    Shift-Tab: 逆方向",
        "  d / e       Source/Assembly切替 / 実行位置へ戻る",
        "  o           関数 / ファイルを開く",
        "  Ctrl-g      プロジェクト全体を検索",
        "  [ / Ctrl-o  閲覧履歴を戻る    ]: 進む",
        "              履歴は表示のみ変更。fはプログラムを実行",
        "",
        "停止条件",
        "  b / B / F   Breakpoint / 条件付き / 関数",
        "  C / w / S   Catchpoint / Watchpoint / 停止条件一覧",
        "",
        "ペイン",
        "  変数         Enter 展開、p 値、a アドレス、E 編集",
        "  レジスタ     p 詳細、E 編集",
        "  スタック     Enterでフレーム選択と位置移動",
        "",
        "モード / その他",
        "  F2          GDB / 読み取り専用VIMモード",
        "  L           表示言語を切り替え（GDBモード）",
        "  VIM         h/j/k/l、/?、n/N、gd/gD、*#、%",
        "  O           プログラム出力",
        "  ?           ヘルプ    q: 終了",
        NULL};
    const char **lines = ui_is_japanese() ? japanese : english;
    int count = 0;
    while (lines[count])
        count++;
    WINDOW *window = create_popup(tui_text("Help", "ヘルプ"), count + 2, 76);
    if (!window)
        return;
    int height, width;
    getmaxyx(window, height, width);
    for (int i = 0; i < count && i + 1 < height - 1; i++) {
        if (lines[i][0] && !isspace((unsigned char)lines[i][0]))
            wattron(window, A_BOLD);
        add_clipped(window, i + 1, 2, lines[i], width - 4);
        wattroff(window, A_BOLD);
    }
    wnoutrefresh(window);
    delwin(window);
}
void draw_output(Gdb *g) {
    erase();
    attron(A_BOLD);
    clipped(0, 2, getmaxx(stdscr) - 4, "%s", tui_text("Program Output", "プログラム出力"));
    attroff(A_BOLD);
    int rows, cols;
    getmaxyx(stdscr, rows, cols);
    int y = 2;
    const char *p = g->output;
    while (*p && y < rows - 2) {
        const char *e = strchr(p, '\n');
        int n = e ? (int)(e - p) : (int)strlen(p);
        mvaddnstr(y++, 1, p, n < cols - 2 ? n : cols - 2);
        if (!e)
            break;
        p = e + 1;
    }
    clipped(rows - 1, 2, cols - 4, "%s", tui_text("Esc / o: Back", "Esc / o: 戻る"));
    refresh();
}
void draw_address(const AddressPanel *p) {
    int rows, cols;
    getmaxyx(stdscr, rows, cols);
    erase();
    int width = cols - 4;
    if (width > 76)
        width = 76;
    if (width < 36)
        width = cols;
    int height = p->info.pointer ? 10 + p->match_count : 7;
    if (p->info.pointer && !p->match_count)
        height = 11;
    if (height > rows - 2)
        height = rows - 2;
    if (height < 7)
        height = 7;
    int x = (cols - width) / 2, y = (rows - height) / 2;
    if (x < 0)
        x = 0;
    if (y < 0)
        y = 0;
    attron(COLOR_PAIR(COLOR_INFO) | A_BOLD);
    mvaddch(y, x, ACS_ULCORNER);
    mvhline(y, x + 1, ACS_HLINE, width - 2);
    mvaddch(y, x + width - 1, ACS_URCORNER);
    for (int r = 1; r < height - 1; r++) {
        mvaddch(y + r, x, ACS_VLINE);
        mvaddch(y + r, x + width - 1, ACS_VLINE);
    }
    mvaddch(y + height - 1, x, ACS_LLCORNER);
    mvhline(y + height - 1, x + 1, ACS_HLINE, width - 2);
    mvaddch(y + height - 1, x + width - 1, ACS_LRCORNER);
    const char *title = p->info.pointer ? tui_text(" Pointer Details ", " ポインタ詳細 ")
                                        : tui_text(" Address ", " アドレス ");
    centered_title(y, x, width, title);
    attroff(COLOR_PAIR(COLOR_INFO) | A_BOLD);
    int row = y + 2;
    clipped(row++, x + 2, width - 4, tui_text("Expression : %s", "式         : %s"),
            p->info.expression);
    clipped(row++, x + 2, width - 4, tui_text("Type       : %s", "型         : %s"), p->info.type);
    if (p->info.pointer) {
        clipped(row++, x + 2, width - 4, tui_text("Address    : %s", "アドレス   : %s"),
                p->info.address);
        clipped(row++, x + 2, width - 4, tui_text("Points to  : %s", "参照先     : %s"),
                p->info.points_to);
        row++;
        attron(A_BOLD | COLOR_PAIR(COLOR_WATCH));
        clipped(row++, x + 2, width - 4, "%s",
                p->match_count == 1 ? tui_text("Matched variable:", "一致する変数:")
                                    : tui_text("Matched variables:", "一致する変数:"));
        attroff(A_BOLD | COLOR_PAIR(COLOR_WATCH));
        if (!p->match_count) {
            clipped(row++, x + 4, width - 6, "%s", tui_text("not found", "見つかりません"));
        } else
            for (int i = 0; i < p->match_count && row < y + height - 1; i++)
                clipped(row++, x + 4, width - 6, "%s", p->matches[i]);
    } else {
        clipped(row++, x + 2, width - 4, tui_text("Value      : %s", "値         : %s"),
                p->info.value);
        clipped(row++, x + 2, width - 4, tui_text("Address    : %s", "アドレス   : %s"),
                p->info.address);
    }
    clipped(rows - 1, 2, cols - 4, "%s",
            tui_text("Esc / a / Enter: Back", "Esc / a / Enter: 戻る"));
    refresh();
}
void build_register_panel(const GdbRegister *r, RegisterPanel *p) {
    memset(p, 0, sizeof(*p));
    snprintf(p->name, sizeof(p->name), "%s", r->name);
    snprintf(p->value, sizeof(p->value), "%s", r->value);
    char *end = NULL;
    p->numeric = strtoull(r->value, &end, 0);
    p->numeric_available = end && end != r->value && (*end == '\0' || isspace((unsigned char)*end));
}
void draw_register_panel(const RegisterPanel *p) {
    int rows, cols;
    getmaxyx(stdscr, rows, cols);
    erase();
    int width = 60;
    if (width > cols - 2)
        width = cols - 2;
    int height = 9, x = (cols - width) / 2, y = (rows - height) / 2;
    if (x < 0)
        x = 0;
    if (y < 0)
        y = 0;
    attron(COLOR_PAIR(COLOR_INFO) | A_BOLD);
    mvaddch(y, x, ACS_ULCORNER);
    mvhline(y, x + 1, ACS_HLINE, width - 2);
    mvaddch(y, x + width - 1, ACS_URCORNER);
    for (int r = 1; r < height - 1; r++) {
        mvaddch(y + r, x, ACS_VLINE);
        mvaddch(y + r, x + width - 1, ACS_VLINE);
    }
    mvaddch(y + height - 1, x, ACS_LLCORNER);
    mvhline(y + height - 1, x + 1, ACS_HLINE, width - 2);
    mvaddch(y + height - 1, x + width - 1, ACS_LRCORNER);
    attroff(COLOR_PAIR(COLOR_INFO) | A_BOLD);
    attron(A_BOLD);
    clipped(y + 2, x + 2, width - 4, tui_text("Register: %s", "レジスタ: %s"), p->name);
    attroff(A_BOLD);
    clipped(y + 3, x + 2, width - 4, tui_text("Value   : %s", "値      : %s"), p->value);
    if (p->numeric_available) {
        clipped(y + 4, x + 2, width - 4, tui_text("Decimal : %llu", "10進数  : %llu"), p->numeric);
        clipped(y + 5, x + 2, width - 4, tui_text("Hex     : 0x%llx", "16進数  : 0x%llx"),
                p->numeric);
    } else
        clipped(y + 4, x + 2, width - 4, "%s",
                tui_text("Numeric value unavailable", "数値を取得できません"));
    clipped(rows - 1, 2, cols - 4, "%s",
            tui_text("Esc / p / Enter: Back", "Esc / p / Enter: 戻る"));
    refresh();
}
void draw_function_dialog(Gdb *g, FunctionDialog *d) {
    int rows, cols;
    getmaxyx(stdscr, rows, cols);
    erase();
    int width = cols - 4;
    if (width > 100)
        width = 100;
    if (width < 48)
        width = cols;
    int height = rows - 4;
    if (height > 20)
        height = 20;
    if (height < 10)
        height = rows;
    int x = (cols - width) / 2, y = (rows - height) / 2;
    if (x < 0)
        x = 0;
    if (y < 0)
        y = 0;
    attron(COLOR_PAIR(COLOR_INFO) | A_BOLD);
    mvaddch(y, x, ACS_ULCORNER);
    mvhline(y, x + 1, ACS_HLINE, width - 2);
    mvaddch(y, x + width - 1, ACS_URCORNER);
    for (int r = 1; r < height - 1; r++) {
        mvaddch(y + r, x, ACS_VLINE);
        mvaddch(y + r, x + width - 1, ACS_VLINE);
    }
    mvaddch(y + height - 1, x, ACS_LLCORNER);
    mvhline(y + height - 1, x + 1, ACS_HLINE, width - 2);
    mvaddch(y + height - 1, x + width - 1, ACS_LRCORNER);
    const char *title = d->action == FUNCTION_OPEN
                            ? tui_text(" Open Function ", " 関数を開く ")
                            : tui_text(" Function Breakpoint ", " 関数Breakpoint ");
    centered_title(y, x, width, title);
    attroff(COLOR_PAIR(COLOR_INFO) | A_BOLD);
    clipped(y + 2, x + 2, width - 4, tui_text("Function: %s_", "関数: %s_"), d->input);
    attron(A_BOLD);
    clipped(y + 4, x + 2, width - 4, "%s", tui_text("Candidates", "候補"));
    attroff(A_BOLD);
    int visible = height - 9;
    if (d->selected < d->top)
        d->top = d->selected;
    if (d->selected >= d->top + visible)
        d->top = d->selected - visible + 1;
    if (!d->count) {
        attron(COLOR_PAIR(COLOR_MUTED) | A_DIM);
        clipped(
            y + 6, x + 3, width - 6, "%s",
            d->input[0]
                ? tui_text("No matching function symbols.", "一致する関数シンボルがありません。")
                : tui_text("No function symbols available.", "関数シンボルを取得できません。"));
        if (!d->input[0])
            clipped(y + 7, x + 3, width - 6, "%s",
                    tui_text("Use address breakpoint in Assembly Mode.",
                             "Assembly ModeでアドレスBreakpointを使用してください。"));
        attroff(COLOR_PAIR(COLOR_MUTED) | A_DIM);
    }
    int module_width = width >= 80 ? 16 : 12;
    int name_width = width - module_width - 27;
    if (name_width < 12)
        name_width = 12;
    for (int row = 0; row < visible; row++) {
        int idx = d->top + row;
        if (idx >= d->count)
            break;
        GdbFunction *f = &d->candidates[idx];
        bool selected = idx == d->selected;
        if (selected)
            attron(A_REVERSE);
        if (d->action == FUNCTION_OPEN) {
            char location[GD_PATH_MAX + 32];
            if (f->source[0] && f->line > 0)
                snprintf(location, sizeof(location), "%s:%d", f->source, f->line);
            else
                snprintf(location, sizeof(location),
                         tui_text("<source unknown> %s", "<ソース不明> %s"),
                         f->address[0] ? f->address : "");
            clipped(y + 5 + row, x + 2, width - 4, "%c %-*.*s  %s", selected ? '>' : ' ',
                    name_width, name_width, f->name, location);
        } else
            clipped(y + 5 + row, x + 2, width - 4, "%c %-*.*s  %-*.*s  %.18s", selected ? '>' : ' ',
                    name_width, name_width, f->name, module_width, module_width,
                    f->module[0] ? f->module : tui_text("<unknown>", "<不明>"),
                    f->address[0] ? f->address : tui_text("<unavailable>", "<取得不可>"));
        if (selected)
            attroff(A_REVERSE);
    }
    attron(COLOR_PAIR(COLOR_MUTED));
    clipped(y + height - 3, x + 2, width - 4,
            tui_text("j/k or arrows: Select   Enter: %s   Esc: Cancel",
                     "j/k・矢印: 選択   Enter: %s   Esc: 中止"),
            d->action == FUNCTION_OPEN ? tui_text("Open", "開く") : tui_text("Set", "設定"));
    attroff(COLOR_PAIR(COLOR_MUTED));
    int color = !strncmp(g->message, "ERROR", 5) || strstr(g->message, "not found") ? COLOR_ERROR
                                                                                    : COLOR_INFO;
    attron(COLOR_PAIR(color));
    clipped(y + height - 2, x + 2, width - 4, "%s", g->message);
    attroff(COLOR_PAIR(color));
    refresh();
}
void draw_open_menu(int selected) {
    int rows, cols;
    getmaxyx(stdscr, rows, cols);
    erase();
    int width = 52, height = 10, x = (cols - width) / 2, y = (rows - height) / 2;
    if (x < 0)
        x = 0;
    if (y < 0)
        y = 0;
    attron(COLOR_PAIR(COLOR_INFO) | A_BOLD);
    mvaddch(y, x, ACS_ULCORNER);
    mvhline(y, x + 1, ACS_HLINE, width - 2);
    mvaddch(y, x + width - 1, ACS_URCORNER);
    for (int r = 1; r < height - 1; r++) {
        mvaddch(y + r, x, ACS_VLINE);
        mvaddch(y + r, x + width - 1, ACS_VLINE);
    }
    mvaddch(y + height - 1, x, ACS_LLCORNER);
    mvhline(y + height - 1, x + 1, ACS_HLINE, width - 2);
    mvaddch(y + height - 1, x + width - 1, ACS_LRCORNER);
    const char *title = tui_text(" Open Source ", " ソースを開く ");
    centered_title(y, x, width, title);
    attroff(COLOR_PAIR(COLOR_INFO) | A_BOLD);
    const char *items[] = {tui_text("Function", "関数"), tui_text("File", "ファイル")};
    for (int i = 0; i < 2; i++) {
        if (i == selected)
            attron(A_REVERSE);
        clipped(y + 3 + i * 2, x + 4, width - 8, "%c %s", i == selected ? '>' : ' ', items[i]);
        if (i == selected)
            attroff(A_REVERSE);
    }
    attron(COLOR_PAIR(COLOR_MUTED));
    clipped(
        y + height - 2, x + 2, width - 4, "%s",
        tui_text("j/k: Select   Enter: Open   Esc: Cancel", "j/k: 選択   Enter: 開く   Esc: 中止"));
    attroff(COLOR_PAIR(COLOR_MUTED));
    refresh();
}
void draw_file_dialog(Gdb *g, FileDialog *d) {
    int rows, cols;
    getmaxyx(stdscr, rows, cols);
    erase();
    int width = cols - 4;
    if (width > 110)
        width = 110;
    if (width < 48)
        width = cols;
    int height = rows - 4;
    if (height > 22)
        height = 22;
    if (height < 10)
        height = rows;
    int x = (cols - width) / 2, y = (rows - height) / 2;
    if (x < 0)
        x = 0;
    if (y < 0)
        y = 0;
    attron(COLOR_PAIR(COLOR_INFO) | A_BOLD);
    mvaddch(y, x, ACS_ULCORNER);
    mvhline(y, x + 1, ACS_HLINE, width - 2);
    mvaddch(y, x + width - 1, ACS_URCORNER);
    for (int r = 1; r < height - 1; r++) {
        mvaddch(y + r, x, ACS_VLINE);
        mvaddch(y + r, x + width - 1, ACS_VLINE);
    }
    mvaddch(y + height - 1, x, ACS_LLCORNER);
    mvhline(y + height - 1, x + 1, ACS_HLINE, width - 2);
    mvaddch(y + height - 1, x + width - 1, ACS_LRCORNER);
    const char *title = tui_text(" Open File ", " ファイルを開く ");
    centered_title(y, x, width, title);
    attroff(COLOR_PAIR(COLOR_INFO) | A_BOLD);
    clipped(y + 2, x + 2, width - 4, tui_text("File: %s_", "ファイル: %s_"), d->input);
    attron(A_BOLD);
    clipped(y + 4, x + 2, width - 4, "%s",
            tui_text("Sources  (debug line info / reference only)",
                     "ソース  （デバッグ行情報あり / 参照のみ）"));
    attroff(A_BOLD);
    int visible = height - 9;
    if (d->selected < d->top)
        d->top = d->selected;
    if (d->selected >= d->top + visible)
        d->top = d->selected - visible + 1;
    if (!d->count) {
        attron(COLOR_PAIR(COLOR_MUTED) | A_DIM);
        clipped(y + 6, x + 3, width - 6, "%s",
                tui_text("No matching project source files.",
                         "一致するプロジェクト内ソースがありません。"));
        attroff(COLOR_PAIR(COLOR_MUTED) | A_DIM);
    }
    for (int row = 0; row < visible; row++) {
        int pos = d->top + row;
        if (pos >= d->count)
            break;
        SourceFileCandidate *f = &d->all[d->matches[pos]];
        bool selected = pos == d->selected;
        if (selected)
            attron(A_REVERSE);
        clipped(y + 5 + row, x + 2, width - 4, "%c [%s] %s", selected ? '>' : ' ',
                f->debug_lines ? tui_text("DEBUG", "DEBUG") : tui_text("REFERENCE", "参照"),
                f->display);
        if (selected)
            attroff(A_REVERSE);
    }
    attron(COLOR_PAIR(COLOR_MUTED));
    clipped(y + height - 3, x + 2, width - 4, "%s",
            tui_text("Type to filter   j/k: Select   Enter: Open   Esc: Cancel",
                     "入力で絞込み   j/k: 選択   Enter: 開く   Esc: 中止"));
    attroff(COLOR_PAIR(COLOR_MUTED));
    attron(COLOR_PAIR(COLOR_INFO));
    clipped(y + height - 2, x + 2, width - 4, "%s", g->message);
    attroff(COLOR_PAIR(COLOR_INFO));
    refresh();
}
void draw_edit_dialog(EditDialog *d) {
    int rows, cols;
    getmaxyx(stdscr, rows, cols);
    erase();
    int width = cols - 4;
    if (width > 76)
        width = 76;
    if (width < 46)
        width = cols;
    int height = 15, x = (cols - width) / 2, y = (rows - height) / 2;
    if (y < 0)
        y = 0;
    if (x < 0)
        x = 0;
    attron(COLOR_PAIR(COLOR_INFO) | A_BOLD);
    mvaddch(y, x, ACS_ULCORNER);
    mvhline(y, x + 1, ACS_HLINE, width - 2);
    mvaddch(y, x + width - 1, ACS_URCORNER);
    for (int r = 1; r < height - 1; r++) {
        mvaddch(y + r, x, ACS_VLINE);
        mvaddch(y + r, x + width - 1, ACS_VLINE);
    }
    mvaddch(y + height - 1, x, ACS_LLCORNER);
    mvhline(y + height - 1, x + 1, ACS_HLINE, width - 2);
    mvaddch(y + height - 1, x + width - 1, ACS_LRCORNER);
    const char *title = d->kind == EDIT_REGISTER ? tui_text(" Edit Register ", " レジスタを編集 ")
                                                 : tui_text(" Edit Variable ", " 変数を編集 ");
    centered_title(y, x, width, title);
    attroff(COLOR_PAIR(COLOR_INFO) | A_BOLD);
    clipped(y + 2, x + 2, width - 4,
            d->kind == EDIT_REGISTER ? tui_text("Register   : %s", "レジスタ   : %s")
                                     : tui_text("Expression : %s", "式         : %s"),
            d->label);
    if (d->type[0])
        clipped(y + 3, x + 2, width - 4, tui_text("Type       : %s", "型         : %s"), d->type);
    clipped(y + 4, x + 2, width - 4, tui_text("Current    : %s", "現在値     : %s"), d->current);
    clipped(y + 6, x + 2, width - 4, tui_text("New value  : %s_", "新しい値   : %s_"), d->input);
    if (d->confirm) {
        attron(COLOR_PAIR(COLOR_COND) | A_BOLD);
        clipped(y + 8, x + 2, width - 4, tui_text("Modify: %s", "変更対象: %s"), d->expression);
        clipped(y + 9, x + 2, width - 4, "%s -> %s", d->current, d->input);
        clipped(y + 11, x + 2, width - 4, "%s", tui_text("Apply? [y/N]", "適用しますか？ [y/N]"));
        attroff(COLOR_PAIR(COLOR_COND) | A_BOLD);
    } else {
        attron(COLOR_PAIR(COLOR_MUTED));
        clipped(y + 11, x + 2, width - 4, "%s",
                tui_text("Enter: Review   Esc: Cancel", "Enter: 確認   Esc: 中止"));
        attroff(COLOR_PAIR(COLOR_MUTED));
    }
    clipped(y + 13, x + 2, width - 4, "%s",
            tui_text("Changes affect only the debugged process.",
                     "変更はデバッグ対象プロセスにだけ反映されます。"));
    refresh();
}
void draw_force_return_dialog(ForceReturnDialog *d) {
    int rows, cols;
    getmaxyx(stdscr, rows, cols);
    erase();
    int width = cols - 4;
    if (width > 78)
        width = 78;
    if (width < 48)
        width = cols;
    int height = 16, x = (cols - width) / 2, y = (rows - height) / 2;
    if (y < 0)
        y = 0;
    if (x < 0)
        x = 0;
    attron(COLOR_PAIR(COLOR_INFO) | A_BOLD);
    mvaddch(y, x, ACS_ULCORNER);
    mvhline(y, x + 1, ACS_HLINE, width - 2);
    mvaddch(y, x + width - 1, ACS_URCORNER);
    for (int r = 1; r < height - 1; r++) {
        mvaddch(y + r, x, ACS_VLINE);
        mvaddch(y + r, x + width - 1, ACS_VLINE);
    }
    mvaddch(y + height - 1, x, ACS_LLCORNER);
    mvhline(y + height - 1, x + 1, ACS_HLINE, width - 2);
    mvaddch(y + height - 1, x + width - 1, ACS_LRCORNER);
    const char *title = tui_text(" Force Return ", " 強制return ");
    centered_title(y, x, width, title);
    attroff(COLOR_PAIR(COLOR_INFO) | A_BOLD);
    clipped(y + 2, x + 2, width - 4, tui_text("Function    : %s()", "関数       : %s()"),
            d->function[0] ? d->function : tui_text("<unknown>", "<不明>"));
    clipped(y + 3, x + 2, width - 4, tui_text("Return type : %s", "戻り値の型 : %s"),
            d->type_known ? d->return_type : tui_text("<unknown>", "<不明>"));
    if (!d->is_void)
        clipped(y + 5, x + 2, width - 4, tui_text("Return value: %s_", "戻り値     : %s_"),
                d->input);
    else
        clipped(y + 5, x + 2, width - 4, "%s",
                tui_text("Return value: <void>", "戻り値     : <void>"));
    if (d->confirm) {
        attron(COLOR_PAIR(COLOR_ERROR) | A_BOLD);
        clipped(y + 8, x + 2, width - 4, "%s",
                tui_text("Current function will not execute remaining instructions.",
                         "現在の関数の残りの命令は実行されません。"));
        clipped(y + 10, x + 2, width - 4,
                tui_text("Force return%s%s? [y/N]", "強制return%s%s？ [y/N]"),
                d->is_void ? "" : " value ", d->is_void ? "" : d->input);
        attroff(COLOR_PAIR(COLOR_ERROR) | A_BOLD);
    } else {
        attron(COLOR_PAIR(COLOR_MUTED));
        clipped(y + 10, x + 2, width - 4, "%s",
                tui_text("Enter: Review   Esc: Cancel", "Enter: 確認   Esc: 中止"));
        attroff(COLOR_PAIR(COLOR_MUTED));
    }
    clipped(y + 13, x + 2, width - 4, "%s",
            tui_text("Control flow of the debugged process will change.",
                     "デバッグ対象プロセスの制御フローが変わります。"));
    refresh();
}
void draw_catch_menu(int selected) {
    int rows, cols;
    getmaxyx(stdscr, rows, cols);
    erase();
    int width = 58, height = 18, x = (cols - width) / 2, y = (rows - height) / 2;
    if (width > cols)
        width = cols;
    if (y < 0)
        y = 0;
    if (x < 0)
        x = 0;
    attron(COLOR_PAIR(COLOR_COND) | A_BOLD);
    mvaddch(y, x, ACS_ULCORNER);
    mvhline(y, x + 1, ACS_HLINE, width - 2);
    mvaddch(y, x + width - 1, ACS_URCORNER);
    for (int r = 1; r < height - 1; r++) {
        mvaddch(y + r, x, ACS_VLINE);
        mvaddch(y + r, x + width - 1, ACS_VLINE);
    }
    mvaddch(y + height - 1, x, ACS_LLCORNER);
    mvhline(y + height - 1, x + 1, ACS_HLINE, width - 2);
    mvaddch(y + height - 1, x + width - 1, ACS_LRCORNER);
    const char *title = tui_text(" Catch Event ", " Catchイベント ");
    centered_title(y, x, width, title);
    attroff(COLOR_PAIR(COLOR_COND) | A_BOLD);
    const char *items[] = {tui_text("Syscall", "システムコール"),
                           tui_text("Signal", "シグナル"),
                           "Fork",
                           "Vfork",
                           "Exec",
                           tui_text("Shared library load", "共有ライブラリ読込み")};
    for (int i = 0; i < 6; i++) {
        if (i == selected)
            attron(A_REVERSE);
        clipped(y + 2 + i * 2, x + 4, width - 8, "%c %s", i == selected ? '>' : ' ', items[i]);
        if (i == selected)
            attroff(A_REVERSE);
    }
    attron(COLOR_PAIR(COLOR_MUTED));
    clipped(y + height - 2, x + 2, width - 4, "%s",
            tui_text("j/k: Select   Enter: Configure   Esc: Cancel",
                     "j/k: 選択   Enter: 設定   Esc: 中止"));
    attroff(COLOR_PAIR(COLOR_MUTED));
    refresh();
}
void draw_catch_search(Gdb *g, CatchDialog *d) {
    int rows, cols;
    getmaxyx(stdscr, rows, cols);
    erase();
    int width = cols - 4;
    if (width > 78)
        width = 78;
    if (width < 44)
        width = cols;
    int height = rows - 4;
    if (height > 20)
        height = 20;
    if (height < 10)
        height = rows;
    int x = (cols - width) / 2, y = (rows - height) / 2;
    if (x < 0)
        x = 0;
    if (y < 0)
        y = 0;
    attron(COLOR_PAIR(COLOR_COND) | A_BOLD);
    mvaddch(y, x, ACS_ULCORNER);
    mvhline(y, x + 1, ACS_HLINE, width - 2);
    mvaddch(y, x + width - 1, ACS_URCORNER);
    for (int r = 1; r < height - 1; r++) {
        mvaddch(y + r, x, ACS_VLINE);
        mvaddch(y + r, x + width - 1, ACS_VLINE);
    }
    mvaddch(y + height - 1, x, ACS_LLCORNER);
    mvhline(y + height - 1, x + 1, ACS_HLINE, width - 2);
    mvaddch(y + height - 1, x + width - 1, ACS_LRCORNER);
    char title[64];
    snprintf(title, sizeof(title), tui_text(" Catch %s ", " Catch %s "), d->event);
    centered_title(y, x, width, title);
    attroff(COLOR_PAIR(COLOR_COND) | A_BOLD);
    clipped(y + 2, x + 2, width - 4, "%s: %s_",
            !strcmp(d->event, "syscall") ? tui_text("Syscall", "システムコール")
                                         : tui_text("Signal", "シグナル"),
            d->input);
    attron(A_BOLD);
    clipped(y + 4, x + 2, width - 4, "%s", tui_text("GDB candidates", "GDBの候補"));
    attroff(A_BOLD);
    int visible = height - 9;
    if (d->selected < d->top)
        d->top = d->selected;
    if (d->selected >= d->top + visible)
        d->top = d->selected - visible + 1;
    if (!d->count) {
        attron(COLOR_PAIR(COLOR_MUTED) | A_DIM);
        clipped(y + 6, x + 3, width - 6,
                tui_text("No matching %s names from GDB.", "GDBに一致する%s名がありません。"),
                d->event);
        attroff(COLOR_PAIR(COLOR_MUTED) | A_DIM);
    }
    for (int row = 0; row < visible; row++) {
        int idx = d->top + row;
        if (idx >= d->count)
            break;
        if (idx == d->selected)
            attron(A_REVERSE);
        clipped(y + 5 + row, x + 2, width - 4, "%c %s", idx == d->selected ? '>' : ' ',
                d->candidates[idx]);
        if (idx == d->selected)
            attroff(A_REVERSE);
    }
    attron(COLOR_PAIR(COLOR_MUTED));
    clipped(y + height - 3, x + 2, width - 4, "%s",
            tui_text("Type to filter   j/k: Select   Enter: Set   Esc: Cancel",
                     "入力で絞込み   j/k: 選択   Enter: 設定   Esc: 中止"));
    attroff(COLOR_PAIR(COLOR_MUTED));
    attron(COLOR_PAIR(!strncmp(g->message, "ERROR", 5) ? COLOR_ERROR : COLOR_INFO));
    clipped(y + height - 2, x + 2, width - 4, "%s", g->message);
    attroff(COLOR_PAIR(!strncmp(g->message, "ERROR", 5) ? COLOR_ERROR : COLOR_INFO));
    refresh();
}
void draw_breaks(Gdb *g, int selected, const char *project_root) {
    int rows, cols;
    erase();
    getmaxyx(stdscr, rows, cols);
    attron(A_BOLD | COLOR_PAIR(COLOR_INFO));
    clipped(0, 2, cols - 4, "%s", tui_text("Stop conditions", "停止条件"));
    attroff(A_BOLD | COLOR_PAIR(COLOR_INFO));
    attron(A_BOLD);
    clipped(2, 1, cols - 2, "%s",
            tui_text("ID       State  Kind         Location / Expression",
                     "ID       状態   種類         場所 / 式"));
    attroff(A_BOLD);
    line_rule(3, NULL);
    if (!g->break_count) {
        attron(COLOR_PAIR(COLOR_MUTED) | A_DIM);
        clipped(5, 2, cols - 4, "%s",
                tui_text("No breakpoints, catchpoints, or watchpoints",
                         "Breakpoint、Catchpoint、Watchpointはありません"));
        attroff(COLOR_PAIR(COLOR_MUTED) | A_DIM);
    }
    for (int i = 0; i < g->break_count && 4 + i < rows - 3; i++) {
        GdbBreakpoint *b = &g->breaks[i];
        bool selected_row = i == selected;
        int color = break_color(b);
        char kind = b->watchpoint ? 'W' : b->catchpoint ? 'C' : 'B';
        char id[24], detail[4096];
        snprintf(id, sizeof(id), "%c#%d", kind, b->number);
        if (b->catchpoint) {
            snprintf(detail, sizeof(detail), "%s%s%s%s", b->catch_type[0] ? b->catch_type : "event",
                     b->catch_target[0] ? "  " : "", b->catch_target,
                     b->number == g->stop_breakpoint ? tui_text("  [last stop]", "  [最終停止]")
                                                     : "");
        } else if (b->watchpoint) {
            bool last = b->number == g->stop_breakpoint && (g->watch_old[0] || g->watch_new[0]);
            snprintf(detail, sizeof(detail), "%s%s%s%s%s%s",
                     b->condition[0] ? b->condition
                                     : tui_text("<expression unavailable>", "<式を取得できません>"),
                     last ? "  [" : "", last ? (g->watch_old[0] ? g->watch_old : "?") : "",
                     last ? " -> " : "", last ? (g->watch_new[0] ? g->watch_new : "?") : "",
                     last ? "]" : "");
        } else if (b->function_breakpoint) {
            snprintf(detail, sizeof(detail), "%s%s%s%s%s",
                     b->function[0] ? b->function : b->original_location,
                     b->pending ? tui_text("  [pending]", "  [保留]") : "",
                     b->address[0] && !b->pending ? "  @ " : "",
                     b->address[0] && !b->pending ? b->address : "",
                     b->file[0] ? tui_text("  [source available]", "  [ソースあり]") : "");
        } else if (b->file[0] && b->line > 0) {
            snprintf(detail, sizeof(detail), "[%c] %s:%d%s%s",
                     project_path(b->file, project_root) ? 'P' : 'X', b->file[0] ? b->file : "?",
                     b->line, b->condition[0] ? "  if " : "", b->condition);
        } else {
            snprintf(detail, sizeof(detail), "[A] *%s%s%s",
                     b->address[0] ? b->address
                                   : tui_text("<address unavailable>", "<アドレス取得不可>"),
                     b->condition[0] ? "  if " : "", b->condition);
        }
        if (selected_row)
            attron(A_REVERSE);
        if (!b->enabled)
            attron(A_DIM);
        attron(COLOR_PAIR(color) | A_BOLD);
        clipped(4 + i, 1, 8, "%-7s", id);
        attroff(COLOR_PAIR(color) | A_BOLD);
        clipped(4 + i, 10, 7, "%-6s",
                b->enabled ? tui_text("on", "有効") : tui_text("off", "無効"));
        clipped(4 + i, 17, 13, "%-12s",
                b->catchpoint                  ? b->catch_type
                : b->watchpoint                ? tui_text("watch", "監視")
                : b->function_breakpoint       ? tui_text("function", "関数")
                : (!b->file[0] || b->line < 1) ? tui_text("address", "アドレス")
                : b->condition[0]              ? tui_text("conditional", "条件付き")
                                               : tui_text("break", "行"));
        clipped(4 + i, 30, cols - 31, "%s", detail);
        if (!b->enabled)
            attroff(A_DIM);
        if (selected_row)
            attroff(A_REVERSE);
    }
    line_rule(rows - 2, NULL);
    attron(COLOR_PAIR(COLOR_MUTED));
    clipped(rows - 1, 1, cols - 2, "%s",
            tui_text("j/k Select  d Delete  e Toggle  Enter Jump  Esc/S Back",
                     "j/k 選択  d 削除  e 有効/無効  Enter 移動  Esc/S 戻る"));
    attroff(COLOR_PAIR(COLOR_MUTED));
    refresh();
}
