#ifndef GD_CONDITION_H
#define GD_CONDITION_H

#include "ui_types.h"

#define CONDITION_CANDIDATES 128
typedef struct {
    char expression[512], type[256], value[GD_TEXT_MAX];
    const char *group;
    bool current;
} ConditionCandidate;

int condition_candidates(Gdb *g, VarRow *visible, int visible_count, ConditionCandidate *items);

void condition_dialog(Gdb *g, Source *source, int *cursor, const char *initial, VarRow *visible,
                      int visible_count);

#endif
