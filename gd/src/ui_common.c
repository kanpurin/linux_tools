#include "ui_common.h"
#include <langinfo.h>
#include <locale.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <wchar.h>

static bool ui_japanese;
const char *tui_text(const char *english, const char *japanese) {
    return ui_japanese ? japanese : english;
}
bool parse_language(const char *value, Language *result) {
    if (!strcasecmp(value, "auto"))
        *result = LANGUAGE_AUTO;
    else if (!strcasecmp(value, "ja") || !strcasecmp(value, "japanese"))
        *result = LANGUAGE_JA;
    else if (!strcasecmp(value, "en") || !strcasecmp(value, "english"))
        *result = LANGUAGE_EN;
    else
        return false;
    return true;
}
bool locale_is_utf8(void) {
    const char *codeset = nl_langinfo(CODESET);
    return codeset && (!strcasecmp(codeset, "UTF-8") || !strcasecmp(codeset, "UTF8"));
}
bool language_is_japanese(Language language) {
    if (!locale_is_utf8())
        return false;
    if (language == LANGUAGE_JA)
        return true;
    if (language == LANGUAGE_EN)
        return false;
    return true; /* UTF-8 can display Japanese even with C/en message locales. */
}
int display_width(const char *text) {
    wchar_t wide[4096];
    size_t length = mbstowcs(wide, text, sizeof(wide) / sizeof(wide[0]) - 1);
    if (length == (size_t)-1)
        return (int)strlen(text);
    int cells = 0;
    for (size_t i = 0; i < length; i++) {
        int width = wcwidth(wide[i]);
        cells += width < 0 ? 1 : width;
    }
    return cells;
}
const char *state_name(GdbState s) {
    switch (s) {
    case GDB_NOT_STARTED:
        return tui_text("NOT STARTED", "未実行");
    case GDB_RUNNING:
        return tui_text("RUNNING", "実行中");
    case GDB_STOPPED:
        return tui_text("STOPPED", "停止中");
    case GDB_EXITED:
        return tui_text("EXITED", "終了");
    default:
        return tui_text("ERROR", "エラー");
    }
}
void add_clipped(WINDOW *window, int y, int x, const char *text, int width) {
    wchar_t wide[8192];
    if (width <= 0)
        return;
    size_t length = mbstowcs(wide, text, sizeof(wide) / sizeof(wide[0]) - 1);
    if (length == (size_t)-1) {
        mvwaddnstr(window, y, x, text, width);
        return;
    }
    int cells = 0;
    size_t i;
    for (i = 0; i < length; i++) {
        int character_width = wcwidth(wide[i]);
        if (character_width < 0)
            character_width = 1;
        if (cells + character_width > width)
            break;
        cells += character_width;
    }
    wide[i] = L'\0';
    mvwaddnwstr(window, y, x, wide, (int)i);
}
void clipped(int y, int x, int width, const char *fmt, ...) {
    char b[8192];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof(b), fmt, ap);
    va_end(ap);
    add_clipped(stdscr, y, x, b, width);
}
const char *focus_name(PaneFocus focus) {
    return focus == PANE_SOURCE      ? tui_text("Code", "コード")
           : focus == PANE_VARIABLES ? tui_text("Variables", "変数")
           : focus == PANE_REGISTERS ? tui_text("Registers", "レジスタ")
                                     : tui_text("Stack", "スタック");
}
PaneFocus cycle_focus(PaneFocus focus, CodeView code_view, int direction) {
    PaneFocus panes[3] = {PANE_SOURCE, code_view == CODE_ASSEMBLY ? PANE_REGISTERS : PANE_VARIABLES,
                          PANE_STACK};
    int current = 0;
    for (int i = 0; i < 3; i++)
        if (panes[i] == focus)
            current = i;
    current = (current + direction + 3) % 3;
    return panes[current];
}
void layout_heights(int rows, int *source_h, int *variables_h, int *stack_h) {
    int available = rows - 7;
    *source_h = available * 55 / 100;
    if (*source_h < 4)
        *source_h = 4;
    int rest = available - *source_h;
    *variables_h = rest / 2;
    if (*variables_h < 3)
        *variables_h = 3;
    *stack_h = rest - *variables_h;
    if (*stack_h < 2)
        *stack_h = 2;
    while (3 + *source_h + 1 + *variables_h + 1 + *stack_h > rows - 2) {
        if (*source_h > 4)
            (*source_h)--;
        else if (*variables_h > 3)
            (*variables_h)--;
        else if (*stack_h > 2)
            (*stack_h)--;
        else
            break;
    }
}
int execution_context_top(int cursor, int source_h) {
    int context = source_h / 4;
    if (context < 2)
        context = 2;
    if (context > 5)
        context = 5;
    int top = cursor - context;
    return top > 0 ? top : 0;
}
void line_rule(int y, const char *title) {
    int cols = getmaxx(stdscr);
    attron(COLOR_PAIR(COLOR_MUTED) | A_DIM);
    mvhline(y, 0, ACS_HLINE, cols);
    attroff(COLOR_PAIR(COLOR_MUTED) | A_DIM);
    if (title) {
        char b[512];
        snprintf(b, sizeof(b), " %s ", title);
        attron(A_BOLD | COLOR_PAIR(COLOR_INFO));
        clipped(y, 2, cols - 4, "%s", b);
        attroff(A_BOLD | COLOR_PAIR(COLOR_INFO));
    }
}
void centered_title(int y, int x, int width, const char *title) {
    int title_width = display_width(title);
    clipped(y, x + (width - title_width) / 2, title_width, "%s", title);
}
WINDOW *create_popup(const char *title, int wanted_height, int wanted_width) {
    int rows, cols, height, width, x, y;
    getmaxyx(stdscr, rows, cols);
    height = wanted_height < rows - 2 ? wanted_height : rows - 2;
    width = wanted_width < cols - 2 ? wanted_width : cols - 2;
    if (height < 5)
        height = rows;
    if (width < 20)
        width = cols;
    y = (rows - height) / 2;
    x = (cols - width) / 2;
    WINDOW *window = newwin(height, width, y, x);
    if (!window)
        return NULL;
    keypad(window, TRUE);
    box(window, 0, 0);
    int title_width = display_width(title);
    if (width > title_width + 4) {
        char decorated[256];
        snprintf(decorated, sizeof(decorated), " %s ", title);
        add_clipped(window, 0, (width - title_width - 2) / 2, decorated, title_width + 2);
    }
    return window;
}
bool prompt_text(const char *label, char *out, size_t size) {
    if (!size)
        return false;
    out[0] = '\0';
    timeout(-1);
    noecho();
    curs_set(1);
    bool accepted = false;
    for (;;) {
        int rows, cols;
        getmaxyx(stdscr, rows, cols);
        move(rows - 1, 0);
        clrtoeol();
        clipped(rows - 1, 0, cols - 1, "%s", label);
        int x = display_width(label);
        if (x > cols - 2)
            x = cols - 2;
        if (x < 0)
            x = 0;
        size_t start = 0, len = strlen(out);
        while (start < len && display_width(out + start) > cols - x - 2) {
            start++;
            while (start < len && ((unsigned char)out[start] & 0xc0) == 0x80)
                start++;
        }
        clipped(rows - 1, x, cols - x - 1, "%s", out + start);
        move(rows - 1, x + display_width(out + start));
        refresh();
        int ch = getch();
        if (ch == 27 || ch == ERR) {
            out[0] = '\0';
            move(rows - 1, 0);
            clrtoeol();
            break;
        }
        if (ch == '\n' || ch == '\r' || ch == KEY_ENTER) {
            accepted = out[0] != '\0';
            break;
        }
        if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) {
            if (len) {
                len--;
                while (len && ((unsigned char)out[len] & 0xc0) == 0x80)
                    len--;
                out[len] = '\0';
            }
        } else if (ch == 21)
            out[0] = '\0';
        else if (ch >= 32 && ch <= 255 && len + 1 < size) {
            out[len] = (char)ch;
            out[len + 1] = '\0';
        }
    }
    curs_set(0);
    timeout(80);
    return accepted;
}
bool confirm_quit(const Gdb *g) {
    char a[8] = "";
    return prompt_text(g->attached_pid
                           ? tui_text("Detach and leave the process running? [y/N] ",
                                      "接続を解除してプロセスを続行しますか？ [y/N] ")
                           : tui_text("Debuggee is active. Quit? [y/N] ",
                                      "デバッグ対象が動作中です。終了しますか？ [y/N] "),
                       a, sizeof(a)) &&
           (a[0] == 'y' || a[0] == 'Y');
}

bool ui_is_japanese(void) {
    return ui_japanese;
}
void ui_set_japanese(bool japanese) {
    ui_japanese = japanese;
}
