#ifndef GD_GDB_CACHE_H
#define GD_GDB_CACHE_H

#include "gdb.h"

typedef struct {
    unsigned long long from, to;
    char name[GD_PATH_MAX];
} GdbModuleRange;

typedef struct {
    char expression[512], address[128];
} CachedAddress;

struct GdbCache {
    unsigned long value_revision;
    CachedAddress addresses[256];
    int address_count;
    GdbAddressInfo details[16];
    int detail_count;
    unsigned long module_revision;
    GdbModuleRange modules[128];
    int module_count;
    bool modules_valid;
    unsigned long source_revision;
    GdbSourceFile sources[512];
    int source_count, source_limit;
    bool sources_valid;
};

struct GdbCache *gdb_cache(Gdb *g);
void gdb_invalidate_values(Gdb *g);
void gdb_invalidate_symbols(Gdb *g);
bool gdb_cacheable_expression(const char *expression);

#endif
