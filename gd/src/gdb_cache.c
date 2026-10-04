#include "gdb_cache.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

struct GdbCache *gdb_cache(Gdb *g) {
    if (!g->cache)
        g->cache = calloc(1, sizeof(*g->cache));
    if (g->cache && g->cache->value_revision != g->data_revision) {
        g->cache->address_count = g->cache->detail_count = 0;
        g->cache->value_revision = g->data_revision;
    }
    return g->cache;
}

void gdb_invalidate_values(Gdb *g) {
    g->data_revision++;
    g->frames_valid = false;
}

void gdb_invalidate_symbols(Gdb *g) {
    g->symbol_revision++;
}

/* Cache eligibility only: evaluation and pointer arithmetic remain GDB's job.
   Calls, assignments and increment/decrement expressions must execute each time. */
bool gdb_cacheable_expression(const char *expression) {
    if (!expression || !*expression || strstr(expression, "++") || strstr(expression, "--") ||
        strpbrk(expression, "=;{}\n"))
        return false;
    int previous = 0;
    for (const unsigned char *p = (const unsigned char *)expression; *p; p++) {
        if (*p == '(' &&
            (isalnum(previous) || previous == '_' || previous == ']' || previous == ')'))
            return false;
        if (!isspace(*p))
            previous = *p;
    }
    return true;
}
