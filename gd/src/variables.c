#include "variables.h"
#include "ui_common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int expansion_index(VarExpansion *items, int count, const char *expression) {
    for (int i = 0; i < count; i++)
        if (!strcmp(items[i].expression, expression))
            return i;
    return -1;
}
static void add_var_header(VarRow *rows, int *row_count, const char *label) {
    if (*row_count >= UI_MAX_VAR_ROWS)
        return;
    VarRow *r = &rows[(*row_count)++];
    memset(r, 0, sizeof(*r));
    r->header = true;
    snprintf(r->label, sizeof(r->label), "%s", label);
}
static void add_var_tree(VarRow *rows, int *row_count, VarExpansion *expansions,
                         int expansion_count, const char *expression, const char *label,
                         const char *value, const char *type, int depth, bool expandable,
                         unsigned long revision) {
    if (*row_count >= UI_MAX_VAR_ROWS || depth > 8)
        return;
    VarRow *r = &rows[(*row_count)++];
    memset(r, 0, sizeof(*r));
    snprintf(r->expression, sizeof(r->expression), "%s", expression);
    snprintf(r->label, sizeof(r->label), "%s", label);
    snprintf(r->value, sizeof(r->value), "%s", value);
    snprintf(r->type, sizeof(r->type), "%s", type ? type : "");
    r->depth = depth;
    r->revision = revision;
    r->expandable = expandable;
    int idx = expansion_index(expansions, expansion_count, expression);
    r->expanded = idx >= 0;
    if (idx >= 0)
        for (int i = 0; i < expansions[idx].child_count; i++) {
            GdbChild *c = &expansions[idx].children[i];
            add_var_tree(rows, row_count, expansions, expansion_count, c->expression, c->name,
                         c->value, c->type, depth + 1, c->numchild > 0, expansions[idx].revision);
        }
}
int build_var_rows(Gdb *g, VarExpansion *expansions, int expansion_count, VarRow *rows) {
    int count = 0;
    if (g->arg_count) {
        add_var_header(rows, &count, tui_text("Args", "引数"));
        for (int i = 0; i < g->arg_count; i++)
            add_var_tree(rows, &count, expansions, expansion_count, g->args[i].name,
                         g->args[i].name, g->args[i].value, g->args[i].type, 0, true,
                         g->variables_revision);
    }
    if (g->local_count) {
        add_var_header(rows, &count, tui_text("Locals", "ローカル変数"));
        for (int i = 0; i < g->local_count; i++)
            add_var_tree(rows, &count, expansions, expansion_count, g->locals[i].name,
                         g->locals[i].name, g->locals[i].value, g->locals[i].type, 0, true,
                         g->variables_revision);
    }
    return count;
}
int selectable_var(VarRow *rows, int count, int start, int direction) {
    for (int i = start; i >= 0 && i < count; i += direction)
        if (!rows[i].header)
            return i;
    return -1;
}
static void collapse_expansion(VarExpansion *items, int *count, int index) {
    if (index < 0 || index >= *count)
        return;
    memmove(&items[index], &items[index + 1], (size_t)(*count - index - 1) * sizeof(*items));
    (*count)--;
}
void toggle_expansion(Gdb *g, VarExpansion *items, int *count, const char *expression) {
    int idx = expansion_index(items, *count, expression);
    if (idx >= 0) {
        collapse_expansion(items, count, idx);
        snprintf(g->message, sizeof(g->message),
                 tui_text("Collapsed %.900s", "折りたたみました: %.900s"), expression);
        return;
    }
    if (*count >= UI_MAX_EXPANSIONS) {
        snprintf(g->message, sizeof(g->message), "%s",
                 tui_text("Too many expanded variables", "展開できる変数の上限に達しました"));
        return;
    }
    VarExpansion *e = &items[*count];
    memset(e, 0, sizeof(*e));
    snprintf(e->expression, sizeof(e->expression), "%s", expression);
    e->revision = g->data_revision;
    if (!gdb_list_children(g, expression, e->children, UI_MAX_CHILDREN, &e->child_count) &&
        e->child_count > 0)
        (*count)++;
}
void refresh_expansions(Gdb *g, VarExpansion *items, int *count) {
    for (int i = 0; i < *count;) {
        int child_count = 0;
        if (gdb_list_children(g, items[i].expression, items[i].children, UI_MAX_CHILDREN,
                              &child_count) ||
            child_count <= 0) {
            collapse_expansion(items, count, i);
            continue;
        }
        items[i].child_count = child_count;
        items[i].revision = g->data_revision;
        i++;
    }
}
int build_address_panel(Gdb *g, VarRow *rows, int row_count, int selected, AddressPanel *panel) {
    memset(panel, 0, sizeof(*panel));
    if (selected < 0 || selected >= row_count || rows[selected].header)
        return -1;
    if (gdb_inspect_address(g, rows[selected].expression, &panel->info))
        return -1;
    if (panel->info.pointer && panel->info.address_available && !panel->info.is_null &&
        !panel->info.optimized_out) {
        for (int i = 0; i < row_count && panel->match_count < UI_MAX_ADDRESS_MATCHES; i++) {
            if (rows[i].header || strstr(rows[i].value, "<optimized out>"))
                continue;
            char address[128];
            if (gdb_expression_address(g, rows[i].expression, address, sizeof(address)) ||
                strcmp(address, panel->info.address))
                continue;
            bool duplicate = false;
            for (int j = 0; j < panel->match_count; j++)
                if (!strcmp(panel->matches[j], rows[i].expression)) {
                    duplicate = true;
                    break;
                }
            if (!duplicate)
                snprintf(panel->matches[panel->match_count++], sizeof(panel->matches[0]), "%.500s",
                         rows[i].expression);
        }
    }
    snprintf(g->message, sizeof(g->message),
             tui_text("Address details: %.700s%s", "アドレス詳細: %.700s%s"),
             panel->info.expression,
             panel->info.pointer ? tui_text(" (pointer)", "（ポインタ）") : "");
    return 0;
}
