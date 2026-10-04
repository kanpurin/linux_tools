#include "dialogs.h"
#include "ui_common.h"

#include <stdio.h>
#include <string.h>

void refresh_function_dialog(Gdb *g, FunctionDialog *d) {
    if (d->complete && d->symbol_revision == g->symbol_revision &&
        strstr(d->input, d->base_filter)) {
        d->count = 0;
        for (int i = 0; i < d->base_count; i++)
            if (strstr(d->base[i].name, d->input))
                d->candidates[d->count++] = d->base[i];
        if (d->count)
            snprintf(g->message, sizeof(g->message),
                     tui_text("%d function candidate%s", "関数候補: %d件%s"), d->count,
                     g->japanese     ? ""
                     : d->count == 1 ? ""
                                     : "s");
        else
            snprintf(
                g->message, sizeof(g->message), "%s",
                d->input[0]
                    ? tui_text("No matching function symbols.",
                               "一致する関数シンボルがありません。")
                    : tui_text(
                          "No function symbols available. Use address breakpoint in Assembly Mode.",
                          "関数シンボルを取得できません。Assembly "
                          "ModeでアドレスBreakpointを使用してください。"));
    } else {
        if (gdb_list_functions(g, d->input, d->candidates, GD_MAX_FUNCTIONS, &d->count))
            d->count = 0;
        d->complete = g->function_results_complete;
        if (d->complete) {
            d->base_count = d->count;
            memcpy(d->base, d->candidates, (size_t)d->count * sizeof(d->base[0]));
            snprintf(d->base_filter, sizeof(d->base_filter), "%s", d->input);
            d->symbol_revision = g->symbol_revision;
        }
    }
    d->selected = 0;
    d->top = 0;
}
void refresh_catch_dialog(Gdb *g, CatchDialog *d) {
    if (gdb_list_catch_candidates(g, d->event, d->input, d->candidates, GD_MAX_CATCH_CANDIDATES,
                                  &d->count))
        d->count = 0;
    d->selected = 0;
    d->top = 0;
}
