#include "condition.h"
#include "source.h"
#include "stops.h"
#include "ui_common.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The last expression token is replaced by Tab; the rest of the condition stays. */
static size_t condition_token(const char *input) {
    size_t start = strlen(input);
    while (start) {
        unsigned char c = (unsigned char)input[start - 1];
        if (isalnum(c) || c == '_' || c == '.' || c == '[' || c == ']' || c == '(' || c == ')')
            start--;
        else if (c == '>' && start >= 2 && input[start - 2] == '-')
            start -= 2;
        else
            break;
    }
    return start;
}

static void condition_add(ConditionCandidate *items, int *count, const char *expression,
                          const char *type, const char *value, const char *group, bool current) {
    if (*count >= CONDITION_CANDIDATES)
        return;
    for (int i = 0; i < *count; i++)
        if (!strcmp(items[i].expression, expression))
            return;
    ConditionCandidate *c = &items[(*count)++];
    snprintf(c->expression, sizeof(c->expression), "%s", expression);
    snprintf(c->type, sizeof(c->type), "%s", type);
    snprintf(c->value, sizeof(c->value), "%s", value);
    c->group = group;
    c->current = current;
}

int condition_candidates(Gdb *g, VarRow *visible, int visible_count, ConditionCandidate *base) {
    int base_count = 0;
    if (g->state == GDB_STOPPED && g->variables_revision != g->data_revision)
        gdb_refresh_variables(g);
    for (int i = 0; i < g->arg_count; i++)
        condition_add(base, &base_count, g->args[i].name, g->args[i].type, g->args[i].value,
                      tui_text("Arg", "引数"), true);
    for (int i = 0; i < g->local_count; i++)
        condition_add(base, &base_count, g->locals[i].name, g->locals[i].type, g->locals[i].value,
                      tui_text("Local", "ローカル"), true);
    for (int i = 0; i < visible_count; i++)
        if (!visible[i].header && visible[i].depth)
            condition_add(base, &base_count, visible[i].expression, visible[i].type,
                          visible[i].value, tui_text("Member", "メンバ"),
                          visible[i].revision == g->data_revision);
    if (g->state == GDB_STOPPED)
        for (int i = 0; i < base_count; i++) {
            if (base[i].current && base[i].value[0] && strcmp(base[i].value, "?") &&
                strcmp(base[i].value, "{...}") && !strstr(base[i].type, "volatile"))
                continue;
            char value[GD_TEXT_MAX];
            if (!gdb_print(g, base[i].expression, value, sizeof(value)))
                snprintf(base[i].value, sizeof(base[i].value), "%s", value);
        }
    return base_count;
}

void condition_dialog(Gdb *g, Source *source, int *cursor, const char *initial, VarRow *visible,
                      int visible_count) {
    static ConditionCandidate base[CONDITION_CANDIDATES], members[CONDITION_CANDIDATES];
    static GdbChild children[GD_MAX_CHILDREN];
    int base_count = 0, member_count = 0, selected = 0, top = 0;
    char input[1024] = "", cached_parent[512] = "", status[GD_TEXT_MAX] = "";
    bool cached_pointer = false;
    unsigned long member_revision = g->data_revision;
    snprintf(input, sizeof(input), "%s", initial ? initial : "");
    int requested = *cursor + 1;
    if (g->state == GDB_RUNNING) {
        snprintf(g->message, sizeof(g->message), "%s",
                 tui_text("Stop the program before editing conditions",
                          "条件を編集する前にプログラムを停止してください"));
        return;
    }
    bool offline = g->state != GDB_STOPPED;
    if (!offline)
        base_count = condition_candidates(g, visible, visible_count, base);
    if (offline) {
        int count = 0;
        base_count = 0;
        if (!gdb_scope_candidates(g, source->path, requested, "", children, GD_MAX_CHILDREN,
                                  &count)) {
            for (int i = 0; i < count; i++)
                condition_add(base, &base_count, children[i].expression, children[i].type,
                              children[i].value, tui_text("Scope", "選択行"), false);
        } else
            snprintf(status, sizeof(status), "%s", g->message);
    }
    for (;;) {
        size_t token_start = condition_token(input);
        const char *token = input + token_start;
        char parent[512] = "";
        const char *filter = token;
        bool pointer = false;
        const char *split = NULL;
        for (const char *p = token; *p; p++) {
            if (*p == '.') {
                split = p;
                pointer = false;
            } else if (*p == '-' && p[1] == '>') {
                split = p;
                pointer = true;
                p++;
            }
        }
        if (split) {
            size_t n = (size_t)(split - token);
            if (n >= sizeof(parent))
                n = sizeof(parent) - 1;
            memcpy(parent, token, n);
            parent[n] = 0;
            filter = split + (pointer ? 2 : 1);
        }
        if (strcmp(parent, cached_parent) || pointer != cached_pointer ||
            (!offline && member_revision != g->data_revision)) {
            member_revision = g->data_revision;
            snprintf(cached_parent, sizeof(cached_parent), "%s", parent);
            cached_pointer = pointer;
            member_count = 0;
            if (parent[0]) {
                int child_count = 0;
                int result =
                    offline ? gdb_scope_candidates(g, source->path, requested, parent, children,
                                                   GD_MAX_CHILDREN, &child_count)
                            : gdb_list_children(g, parent, children, GD_MAX_CHILDREN, &child_count);
                if (!result) {
                    for (int i = 0; i < child_count; i++)
                        condition_add(members, &member_count, children[i].expression,
                                      children[i].type, children[i].value,
                                      tui_text("Member", "メンバ"), true);
                } else
                    snprintf(status, sizeof(status), "%s", g->message);
            }
        }
        ConditionCandidate *items = parent[0] ? members : base;
        int count = parent[0] ? member_count : base_count, matches[CONDITION_CANDIDATES],
            match_count = 0;
        for (int i = 0; i < count; i++) {
            const char *name = items[i].expression;
            if (parent[0]) {
                const char *last = NULL;
                for (const char *p = name; *p; p++) {
                    if (*p == '.')
                        last = p;
                    else if (*p == '-' && p[1] == '>')
                        last = p + 1;
                }
                if (last)
                    name = last + 1;
            }
            if (!strncmp(name, filter, strlen(filter)))
                matches[match_count++] = i;
        }
        if (selected >= match_count)
            selected = match_count ? match_count - 1 : 0;
        int rows = getmaxy(stdscr);
        erase();
        WINDOW *w =
            create_popup(tui_text("Conditional Breakpoint", "条件付きBreakpoint"), rows - 2, 110);
        if (!w)
            break;
        int height, width;
        getmaxyx(w, height, width);
        if (height < 14 || width < 50) {
            delwin(w);
            snprintf(g->message, sizeof(g->message), "%s",
                     tui_text("Condition editor needs a larger terminal (50x16)",
                              "条件編集には50列×16行以上の端末が必要です"));
            break;
        }
        char line[2048];
        snprintf(line, sizeof(line), tui_text("Location: %s:%d", "設定位置: %s:%d"),
                 path_name(source->path), requested);
        add_clipped(w, 1, 2, line, width - 4);
        if (offline)
            snprintf(
                line, sizeof(line), "%s",
                tui_text("Candidates: debug symbols at selected line; runtime values unavailable",
                         "候補: 選択行のデバッグ情報（実行前のため現在値なし）"));
        else
            snprintf(
                line, sizeof(line),
                tui_text("Candidates: current frame #%d %s() ONLY",
                         "候補: 現在フレーム #%d %s() の変数（設定位置で使えるとは限りません）"),
                g->selected_frame, g->function);
        add_clipped(w, 2, 2, line, width - 4);
        add_clipped(
            w, 3, 2,
            tui_text(
                "Enter: GDB validates at breakpoint location. Ctrl-v: evaluate here (may have side "
                "effects).",
                "Enter: 設定位置でGDBが検証。Ctrl-v: 現在フレームで評価（副作用のある式に注意）"),
            width - 4);
        wattron(w, A_BOLD | COLOR_PAIR(COLOR_INFO));
        snprintf(line, sizeof(line), tui_text("Condition: %s_", "条件: %s_"), input);
        /* Keep the tail visible while composing long AND/OR conditions. */
        if (display_width(line) > width - 4)
            snprintf(line, sizeof(line), "...%s_", input + strlen(input) - (size_t)(width - 9));
        add_clipped(w, 5, 2, line, width - 4);
        wattroff(w, A_BOLD | COLOR_PAIR(COLOR_INFO));
        add_clipped(w, 7, 2,
                    tui_text("Expression                          Type / Current value",
                             "式                                  型 / 現在値"),
                    width - 4);
        int page = height - 12;
        if (selected < top)
            top = selected;
        if (selected >= top + page)
            top = selected - page + 1;
        if (!match_count)
            add_clipped(
                w, 8, 2,
                tui_text(
                    "No candidates. Enter an expression manually, or stop in the target frame.",
                    "候補なし。式を手入力するか、対象フレームで停止してください。"),
                width - 4);
        for (int row = 0; row < page && top + row < match_count; row++) {
            ConditionCandidate *c = &items[matches[top + row]];
            if (top + row == selected)
                wattron(w, A_REVERSE);
            snprintf(line, sizeof(line), "%s [%s] %s", top + row == selected ? ">" : " ", c->group,
                     c->expression);
            add_clipped(w, 8 + row, 2, line, width / 2 - 3);
            snprintf(line, sizeof(line), "%s = %s", c->type, c->value);
            add_clipped(w, 8 + row, width / 2, line, width / 2 - 2);
            if (top + row == selected)
                wattroff(w, A_REVERSE);
        }
        add_clipped(w, height - 3, 2, status, width - 4);
        add_clipped(w, height - 2, 2,
                    tui_text("Up/Down: select  Tab: insert  Right: members  Ctrl-u: clear  Enter: "
                             "set  Esc: cancel",
                             "↑↓ 選択  Tab 挿入  → メンバ  Ctrl-u 消去  Enter 設定  Esc 中止"),
                    width - 4);
        wrefresh(w);
        wtimeout(w, -1);
        int ch = wgetch(w);
        delwin(w);
        if (ch == 27) {
            snprintf(
                g->message, sizeof(g->message), "%s",
                tui_text("Conditional breakpoint cancelled", "条件付きBreakpointを中止しました"));
            break;
        }
        if (ch == KEY_UP) {
            if (selected > 0)
                selected--;
            continue;
        }
        if (ch == KEY_DOWN) {
            if (selected + 1 < match_count)
                selected++;
            continue;
        }
        if ((ch == '\t' || ch == KEY_RIGHT) && match_count) {
            ConditionCandidate *c = &items[matches[selected]];
            const char *suffix = ch == KEY_RIGHT ? (strchr(c->type, '*') ? "->" : ".") : "";
            if (token_start + strlen(c->expression) + strlen(suffix) < sizeof(input))
                snprintf(input + token_start, sizeof(input) - token_start, "%s%s", c->expression,
                         suffix);
            selected = top = 0;
            status[0] = 0;
            continue;
        }
        if (ch == 22) {
            if (g->state != GDB_STOPPED)
                snprintf(status, sizeof(status), "%s",
                         tui_text("Current-frame evaluation requires a stopped process",
                                  "現在フレームでの評価には停止が必要です"));
            else if (input[0]) {
                char value[GD_TEXT_MAX];
                if (gdb_print(g, input, value, sizeof(value)))
                    snprintf(status, sizeof(status), "%s", g->message);
                else
                    snprintf(
                        status, sizeof(status),
                        tui_text(
                            "Current frame value: %.900s (not validation for another location)",
                            "現在フレームでの値: %.900s（別の設定位置の検証ではありません）"),
                        value);
                base_count = condition_candidates(g, visible, visible_count, base);
            }
            continue;
        }
        if (ch == '\n' || ch == KEY_ENTER) {
            if (!input[0]) {
                snprintf(status, sizeof(status), "%s",
                         tui_text("Enter a condition", "条件を入力してください"));
                continue;
            }
            int oldmax = newest_break_number(g);
            if (!gdb_set_cond_breakpoint(g, source->path, requested, input)) {
                int actual = new_break_line(g, source->path, oldmax);
                if (actual > 0)
                    *cursor = actual - 1;
                break;
            }
            snprintf(status, sizeof(status), "%s", g->message);
            continue;
        }
        size_t len = strlen(input);
        if (ch == 21)
            input[0] = 0;
        else if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) {
            if (len)
                input[len - 1] = 0;
        } else if (ch >= 32 && ch < 127 && len + 1 < sizeof(input)) {
            input[len] = (char)ch;
            input[len + 1] = 0;
        }
        selected = top = 0;
        status[0] = 0;
    }
    touchwin(stdscr);
    timeout(80);
}
