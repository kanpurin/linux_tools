#ifndef GD_PROJECT_SEARCH_H
#define GD_PROJECT_SEARCH_H

#include "ui_types.h"

void project_search(Gdb *g, const char *root, Source *source, SourceHistory *history, int *cursor,
                    int *column, int *top, int *hscroll, CodeView *code_view, PaneFocus *focus,
                    char *last_search, size_t search_size, bool *search_active, bool *whole_word);

#endif
