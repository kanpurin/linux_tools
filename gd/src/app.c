#include "app.h"
#include "app_state.h"
#include "ui.h"
#include <errno.h>
#include <limits.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int gd_app_run(int argc, char **argv) {
    static App state;
    App *app = &state;
    memset(app, 0, sizeof(*app));
    setlocale(LC_ALL, "");
    Language language = LANGUAGE_AUTO;
    int first = 1;
    pid_t attach_pid = 0;
    while (first < argc) {
        if (!strcmp(argv[first], "--lang")) {
            if (first + 1 >= argc || !parse_language(argv[first + 1], &language)) {
                fprintf(stderr, "gd: --lang must be auto, ja, or en\n");
                return 2;
            }
            first += 2;
        } else if (!strcmp(argv[first], "-p") || !strcmp(argv[first], "--pid")) {
            if (attach_pid || first + 1 >= argc) {
                fprintf(stderr, "gd: specify one positive PID with -p/--pid\n");
                return 2;
            }
            const char *text = argv[first + 1];
            char *end;
            errno = 0;
            long value = strtol(text, &end, 10);
            if (errno || !text[0] || strspn(text, "0123456789") != strlen(text) || *end ||
                value <= 0 || value > INT_MAX) {
                fprintf(stderr, "gd: PID must be a positive integer\n");
                return 2;
            }
            attach_pid = (pid_t)value;
            first += 2;
        } else if (!strcmp(argv[first], "--args") || !strcmp(argv[first], "--")) {
            first++;
            break;
        } else if (!strcmp(argv[first], "--help") || !strcmp(argv[first], "-h")) {
            printf("Usage: gd [--lang auto|ja|en] [--args] PROGRAM [ARG...]\n       gd [--lang "
                   "auto|ja|en] -p PID\n");
            return 0;
        } else
            break;
    }
    ui_set_japanese(language_is_japanese(language));
    if (language == LANGUAGE_JA && !locale_is_utf8())
        fprintf(stderr, "gd: Japanese UI requires a UTF-8 locale; using English\n");
    if (attach_pid && first < argc) {
        fprintf(stderr, "gd: -p/--pid cannot be combined with PROGRAM or arguments\n");
        return 2;
    }
    if (!attach_pid && first >= argc) {
        fprintf(stderr, "gd: missing program or PID\n");
        return 2;
    }
    int start_result =
        attach_pid ? gdb_attach(&app->g, attach_pid)
                   : gdb_start(&app->g, argv[first], argc > first + 1 ? &argv[first + 1] : NULL);
    if (start_result) {
        fprintf(stderr, "gd: failed to %s: %s\n", attach_pid ? "attach" : "start GDB",
                app->g.message);
        gdb_shutdown(&app->g);
        return 1;
    }
    app->g.japanese = ui_is_japanese();
    app->g.defer_disassembly = true;
    if (attach_pid)
        snprintf(
            app->g.message, sizeof(app->g.message),
            tui_text("Attached to PID %ld - press c to continue", "PID %ldに接続しました。cで続行"),
            (long)attach_pid);
    if (ui_is_japanese() && !strcmp(app->g.message, "Ready - press r to run"))
        snprintf(app->g.message, sizeof(app->g.message), "準備完了 - rで実行");
    initscr();
    if (has_colors()) {
        start_color();
        use_default_colors();
        init_pair(COLOR_BREAK, COLOR_CYAN, -1);
        init_pair(COLOR_COND, COLOR_YELLOW, -1);
        init_pair(COLOR_WATCH, COLOR_GREEN, -1);
        init_pair(COLOR_ERROR, COLOR_RED, -1);
        init_pair(COLOR_INFO, COLOR_CYAN, -1);
        init_pair(COLOR_MUTED, COLOR_WHITE, -1);
    }
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    set_escdelay(25);
    curs_set(0);
    timeout(80);
    app->history.index = -1;
    project_root_init(&app->g, app->project_root, sizeof(app->project_root));
    app->view = VIEW_MAIN;
    app->mode = MODE_GDB;
    app->code_view = app->g.source_available ? CODE_SOURCE : CODE_ASSEMBLY;
    app->focus = PANE_SOURCE;
    app->search_direction = 1;
    app->redraw_requested = true;
    while (!app->done) {
        app_update_and_draw(app);
        int ch = getch();
        if (ch == ERR)
            continue;
        app->redraw_requested = true;
        if (!app_handle_view(app, ch))
            app_handle_input(app, ch);
    }
    endwin();
    source_free(&app->src);
    gdb_shutdown(&app->g);
    return 0;
}
