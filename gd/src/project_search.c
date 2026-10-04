#include "project_search.h"
#include "source.h"
#include "stops.h"
#include "ui_common.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define UI_MAX_SEARCH_HITS 1024
typedef struct {
    int file, line, column, break_line;
    char text[512];
} ProjectSearchHit;

void project_search(Gdb *g, const char *root, Source *source, SourceHistory *history, int *cursor,
                    int *column, int *top, int *hscroll, CodeView *code_view, PaneFocus *focus,
                    char *last_search, size_t search_size, bool *search_active, bool *whole_word) {
    static FileDialog files;
    static ProjectSearchHit hits[UI_MAX_SEARCH_HITS];
    char query[256] = "";
    if (g->state == GDB_RUNNING) {
        snprintf(g->message, sizeof(g->message), "%s",
                 tui_text("Stop the program before project search",
                          "プロジェクト検索の前にプログラムを停止してください"));
        return;
    }
retry_search:
    if (!prompt_text(tui_text("Project search (literal): ", "プロジェクト検索（文字列）: "), query,
                     sizeof(query)))
        return;
    load_file_dialog(g, root, &files);
    int count = 0, skipped = 0;
    bool limited = false;
    for (int i = 0; i < files.all_count; i++) {
        FILE *file = fopen(files.all[i].path, "r");
        if (!file) {
            skipped++;
            continue;
        }
        struct stat st;
        if (fstat(fileno(file), &st) || st.st_size > 8 * 1024 * 1024) {
            skipped++;
            fclose(file);
            continue;
        }
        char *line = NULL;
        size_t capacity = 0;
        ssize_t length;
        int line_number = 0;
        while ((length = getline(&line, &capacity, file)) >= 0) {
            line_number++;
            /* Source searches are literal and case-sensitive, one result per line. */
            char *match = strstr(line, query);
            if (!match)
                continue;
            if (count >= UI_MAX_SEARCH_HITS) {
                limited = true;
                break;
            }
            ProjectSearchHit *hit = &hits[count++];
            hit->file = i;
            hit->line = line_number;
            hit->column = (int)(match - line);
            hit->break_line = 0;
            while (length > 0 && (line[length - 1] == '\n' || line[length - 1] == '\r'))
                line[--length] = 0;
            /* Keep the match visible even on a very long source line. */
            int start = hit->column > 80 ? hit->column - 80 : 0;
            snprintf(hit->text, sizeof(hit->text), "%s%s", start ? "..." : "", line + start);
        }
        if (ferror(file))
            skipped++;
        free(line);
        fclose(file);
        if (limited)
            break;
    }
    int selected = 0, result_top = 0;
    char message[GD_TEXT_MAX] = "";
    for (;;) {
        int rows, cols;
        getmaxyx(stdscr, rows, cols);
        erase();
        attron(A_BOLD | COLOR_PAIR(COLOR_INFO));
        clipped(0, 1, cols - 2,
                tui_text("Project search: %s  (%d matching lines)",
                         "プロジェクト検索: %s  （%d行一致）"),
                query, count);
        attroff(A_BOLD | COLOR_PAIR(COLOR_INFO));
        clipped(1, 1, cols - 2, tui_text("Root: %s", "検索範囲: %s"), root);
        clipped(2, 1, cols - 2,
                tui_text("%d source files; skipped %d%s%s", "対象%dファイル・読み飛ばし%d%s%s"),
                files.all_count, skipped,
                limited ? tui_text("; result limit 1024 reached", "・結果上限1024件") : "",
                files.all_count == UI_MAX_SOURCE_FILES
                    ? tui_text("; file limit 512 reached", "・ファイル上限512件")
                    : "");
        line_rule(3, NULL);
        int page = rows - 7;
        if (page < 1)
            page = 1;
        if (selected < result_top)
            result_top = selected;
        if (selected >= result_top + page)
            result_top = selected - page + 1;
        if (!count)
            clipped(5, 2, cols - 4, "%s",
                    tui_text("No matches in project C/C++ sources and headers.",
                             "プロジェクトのC/C++ソース・ヘッダーに一致する行がありません。"));
        for (int row = 0; row < page && result_top + row < count; row++) {
            int index = result_top + row;
            ProjectSearchHit *hit = &hits[index];
            SourceFileCandidate *file = &files.all[hit->file];
            int location_width = cols / 3;
            if (index == selected)
                attron(A_REVERSE);
            clipped(
                4 + row, 1, location_width - 1, "%c %c [%s] %s:%d", index == selected ? '>' : ' ',
                breakpoint_mark(g, file->path, hit->break_line ? hit->break_line : hit->line) ? 'B'
                                                                                              : ' ',
                file->debug_lines ? "D" : "R", file->display, hit->line);
            clipped(4 + row, location_width + 1, cols - location_width - 2, "%s", hit->text);
            if (index == selected)
                attroff(A_REVERSE);
        }
        line_rule(rows - 3, NULL);
        clipped(
            rows - 2, 1, cols - 2, "%s",
            tui_text(
                "j/k or arrows: select  Enter: open  b: breakpoint  /: new search  Esc: back  [D] "
                "debug [R] reference",
                "j/k・↑↓ 選択  Enter 移動  b Breakpoint  / 再検索  Esc 戻る  [D]デバッグ [R]参照"));
        clipped(rows - 1, 1, cols - 2, "%s", message);
        refresh();
        int ch = getch();
        if (ch == ERR)
            continue;
        if (ch == 27)
            break;
        if ((ch == 'j' || ch == KEY_DOWN) && selected + 1 < count)
            selected++;
        else if ((ch == 'k' || ch == KEY_UP) && selected > 0)
            selected--;
        else if (ch == KEY_NPAGE) {
            selected += page;
            if (selected >= count)
                selected = count ? count - 1 : 0;
        } else if (ch == KEY_PPAGE) {
            selected -= page;
            if (selected < 0)
                selected = 0;
        } else if (ch == '/')
            goto retry_search;
        else if (count && (ch == '\n' || ch == KEY_ENTER || ch == 'b')) {
            ProjectSearchHit *hit = &hits[selected];
            SourceFileCandidate *file = &files.all[hit->file];
            if (ch == 'b') {
                if (!file->debug_lines)
                    snprintf(
                        message, sizeof(message), "%s",
                        tui_text(
                            "No debug line info: use a function or address breakpoint",
                            "デバッグ行情報なし。関数またはアドレスBreakpointを使用してください"));
                else {
                    int oldmax = newest_break_number(g);
                    if (!gdb_toggle_breakpoint(g, file->path,
                                               hit->break_line ? hit->break_line : hit->line)) {
                        int actual = new_break_line(g, file->path, oldmax);
                        if (actual > 0)
                            hit->break_line = actual;
                    }
                    snprintf(message, sizeof(message), "%s", g->message);
                }
            } else if (source_navigate(source, history, file->path, hit->line, cursor, column, top,
                                       hscroll, file->debug_lines, true)) {
                *code_view = CODE_SOURCE;
                *focus = PANE_SOURCE;
                *column = hit->column;
                clamp_column(source, *cursor, column);
                snprintf(last_search, search_size, "%s", query);
                *search_active = false;
                *whole_word = false;
                snprintf(g->message, sizeof(g->message),
                         tui_text("Search result: %s:%d", "検索結果: %s:%d"), file->display,
                         hit->line);
                break;
            } else
                snprintf(
                    message, sizeof(message), "%s",
                    tui_text("Source file is no longer available", "ソースファイルを開けません"));
        }
    }
    touchwin(stdscr);
}
