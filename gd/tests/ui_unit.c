#include "../src/app_state.h"
#include "../src/ui.h"

#include <assert.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void search_and_history(void) {
    Source source = {0};
    source.count = 3;
    source.lines = calloc((size_t)source.count, sizeof(*source.lines));
    assert(source.lines);
    source.lines[0] = strdup("int ret = 0;");
    source.lines[1] = strdup("return ret;");
    source.lines[2] = strdup("ret++;");
    for (int i = 0; i < source.count; i++)
        assert(source.lines[i]);
    int line = 0, column = 4;
    assert(text_search_position(&source, &line, &column, "ret", 1, true));
    assert(line == 1 && column == 7); /* Not the 'ret' in 'return'. */
    assert(text_search_position(&source, &line, &column, "ret", 1, true));
    assert(line == 2 && column == 0);
    assert(text_search_position(&source, &line, &column, "ret", 1, true));
    assert(line == 0 && column == 4);
    assert(text_search_position(&source, &line, &column, "ret", -1, true));
    assert(line == 2 && column == 0);
    assert(!text_search_position(&source, &line, &column, "missing", 1, false));
    assert(source_search(&source, 2, "int", 1) == 0);
    char word[32];
    assert(word_under_cursor(&source, 0, 5, word, sizeof(word), NULL));
    assert(!strcmp(word, "ret"));
    source_free(&source);
    assert(source.count == 0 && source.lines == NULL);

    SourceHistory history = {.index = -1};
    history_push(&history, "/project/a.c", 3, 0, 1, 0, true);
    history_push(&history, "/project/a.c", 3, 2, 1, 0, true);
    assert(history.count == 1 && history.items[0].column == 2);
    history_push_assembly(&history, "0x401000", "helper", 0, 0);
    assert(history.count == 2 && history.items[1].code_view == CODE_ASSEMBLY);
    history.index = 0;
    history_push(&history, "/project/b.c", 8, 0, 6, 0, true);
    assert(history.count == 2 && !strcmp(history.items[1].path, "/project/b.c"));
    for (int i = 0; i < UI_HISTORY_MAX + 10; i++)
        history_push(&history, "/project/c.c", i, 0, i, 0, true);
    assert(history.count == UI_HISTORY_MAX && history.index == UI_HISTORY_MAX - 1);
    assert(cycle_focus(PANE_SOURCE, CODE_SOURCE, 1) == PANE_VARIABLES);
    assert(cycle_focus(PANE_SOURCE, CODE_ASSEMBLY, -1) == PANE_STACK);
    assert(execution_context_top(20, 20) == 15);
    assert(execution_context_top(0, 20) == 0);
}

static void variables(void) {
    static Gdb g;
    static VarExpansion expansions[UI_MAX_EXPANSIONS];
    static VarRow rows[UI_MAX_VAR_ROWS];
    g.arg_count = 2;
    g.local_count = 1;
    snprintf(g.args[0].name, sizeof(g.args[0].name), "dev");
    snprintf(g.args[1].name, sizeof(g.args[1].name), "value");
    snprintf(g.locals[0].name, sizeof(g.locals[0].name), "cursor");
    snprintf(expansions[0].expression, sizeof(expansions[0].expression), "dev");
    expansions[0].child_count = 1;
    snprintf(expansions[0].children[0].expression, sizeof(expansions[0].children[0].expression),
             "dev->status");
    snprintf(expansions[0].children[0].name, sizeof(expansions[0].children[0].name), "status");
    snprintf(expansions[1].expression, sizeof(expansions[1].expression), "cursor");
    expansions[1].child_count = 1;
    snprintf(expansions[1].children[0].expression, sizeof(expansions[1].children[0].expression),
             "*cursor");
    snprintf(expansions[1].children[0].name, sizeof(expansions[1].children[0].name), "*cursor");
    ui_set_japanese(false);
    assert(build_var_rows(&g, expansions, 2, rows) == 7);
    assert(rows[0].header && !strcmp(rows[0].label, "Args"));
    assert(!strcmp(rows[1].expression, "dev") && rows[1].expanded);
    assert(!strcmp(rows[2].expression, "dev->status") && rows[2].depth == 1);
    assert(!strcmp(rows[3].expression, "value"));
    assert(rows[4].header && !strcmp(rows[4].label, "Locals"));
    assert(!strcmp(rows[5].expression, "cursor"));
    assert(!strcmp(rows[6].expression, "*cursor"));
    assert(selectable_var(rows, 7, 4, 1) == 5);
    assert(selectable_var(rows, 7, 4, -1) == 3);
    assert(build_var_rows(&g, expansions, 0, rows) == 5);
    ui_set_japanese(true);
    assert(build_var_rows(&g, expansions, 0, rows) == 5);
    assert(!strcmp(rows[0].label, "引数"));
    assert(!strcmp(rows[3].label, "ローカル変数"));
    ui_set_japanese(false);
}

static void input_dispatch(void) {
    static App app;
    app.mode = MODE_GDB;
    app.code_view = CODE_SOURCE;
    app.focus = PANE_SOURCE;
    app.search_active = app.search_whole_word = true;
    app.search_direction = -1;
    app.vim_count = 42;
    snprintf(app.last_search, sizeof(app.last_search), "ret");
    app.view = VIEW_HELP;
    assert(app_handle_view(&app, 27));
    assert(app.view == VIEW_MAIN && app.search_active);
    assert(!app_handle_view(&app, 27));
    app_handle_input(&app, 27);
    assert(!app.search_active && !app.search_whole_word && !app.last_search[0]);
    assert(app.search_direction == 1 && app.vim_count == 0);
    app_handle_input(&app, '\t');
    assert(app.focus == PANE_VARIABLES);
    app_handle_input(&app, KEY_BTAB);
    assert(app.focus == PANE_SOURCE);
    app_handle_input(&app, KEY_F(2));
    assert(app.mode == MODE_VIM);
    app_handle_input(&app, KEY_F(2));
    assert(app.mode == MODE_GDB);
}

static void idle_rendering(void) {
    FILE *output = tmpfile(), *input = tmpfile();
    assert(output && input);
    SCREEN *terminal = newterm("xterm", output, input);
    assert(terminal);
    int pipefd[2];
    assert(!pipe(pipefd));
    static App app;
    app.g.from_gdb = pipefd[0];
    app.g.state = GDB_NOT_STARTED;
    app.history.index = -1;
    app_update_and_draw(&app);
    assert(app.draw_count == 1);
    for (int i = 0; i < 40; i++)
        app_update_and_draw(&app);
    assert(app.draw_count == 1);
    app.redraw_requested = true;
    app_update_and_draw(&app);
    assert(app.draw_count == 2);
    const char event[] = "~\"new output\\n\"\n";
    assert(write(pipefd[1], event, sizeof(event) - 1) == (ssize_t)sizeof(event) - 1);
    app_update_and_draw(&app);
    assert(app.draw_count == 3 && strstr(app.g.output, "new output"));
    app_update_and_draw(&app);
    assert(app.draw_count == 3);
    memset(app.g.output, 'x', sizeof(app.g.output) - 1);
    app.g.output[sizeof(app.g.output) - 1] = '\0';
    app.g.output_len = sizeof(app.g.output) - 1;
    assert(write(pipefd[1], event, sizeof(event) - 1) == (ssize_t)sizeof(event) - 1);
    app_update_and_draw(&app);
    assert(app.draw_count == 4 && app.g.output_len == sizeof(app.g.output) - 1);
    assert(strstr(app.g.output, "new output"));
    resizeterm(40, 100);
    app_update_and_draw(&app);
    assert(app.draw_count == 5 && app.rows == 40 && app.cols == 100);
    close(pipefd[0]);
    close(pipefd[1]);
    endwin();
    delscreen(terminal);
    fclose(output);
    fclose(input);
}

static void large_source(void) {
    char path[] = "/tmp/gd-source-unit-XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);
    FILE *file = fdopen(fd, "w");
    assert(file);
    for (int i = 0; i < 4096; i++)
        fprintf(file, "line %d\n", i);
    assert(!fclose(file));
    Source source = {0};
    assert(!source_load(&source, path) && source.count == 4096);
    assert(!strcmp(source.lines[4095], "line 4095"));
    char **lines = source.lines;
    assert(!source_load(&source, path) && source.lines == lines);
    source_free(&source);
    assert(!unlink(path));
}

static void language_defaults(void) {
    char *saved = strdup(setlocale(LC_ALL, NULL));
    assert(saved && setlocale(LC_ALL, "C"));
    assert(!language_is_japanese(LANGUAGE_AUTO));
    assert(!language_is_japanese(LANGUAGE_JA));
    assert(!language_is_japanese(LANGUAGE_EN));
    assert(setlocale(LC_CTYPE, "C.UTF-8"));
    assert(language_is_japanese(LANGUAGE_AUTO));
    assert(language_is_japanese(LANGUAGE_JA));
    assert(!language_is_japanese(LANGUAGE_EN));
    assert(!strcmp(setlocale(LC_MESSAGES, NULL), "C"));
    assert(setlocale(LC_ALL, saved));
    free(saved);
}

int main(void) {
    language_defaults();
    search_and_history();
    variables();
    input_dispatch();
    idle_rendering();
    large_source();
    puts("UI unit: ok");
    return 0;
}
