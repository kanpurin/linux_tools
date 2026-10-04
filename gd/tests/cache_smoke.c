#include "../src/condition.h"
#include "../src/dialogs.h"
#include "../src/gdb_cache.h"
#include "../src/gdb_internal.h"
#include "../src/source.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void write_source(const char *path, const char *text) {
    FILE *f = fopen(path, "w");
    assert(f && fputs(text, f) >= 0 && !fclose(f));
}

int main(int argc, char **argv) {
    if (argc != 4)
        return 2;
    static Gdb g;
    static FunctionDialog functions;
    static FileDialog files;
    static ConditionCandidate candidates[CONDITION_CANDIDATES];
    assert(!gdb_start(&g, argv[1], NULL));
    g.defer_disassembly = true;
    assert(!gdb_toggle_breakpoint(&g, argv[2], 7));
    assert(!gdb_run(&g));
    for (int i = 0; i < 200 && g.state != GDB_STOPPED; i++)
        assert(gdb_poll(&g, 50) >= 0);
    assert(g.state == GDB_STOPPED && !g.instruction_count);
    int token = g.token;
    assert(!gdb_select_frame(&g, 1));
    assert(g.token - token == 5 && g.arg_count == 2 && g.local_count == 1);
    token = g.token;
    assert(condition_candidates(&g, NULL, 0, candidates) == 3);
    assert(g.token == token); /* Fresh scalar/pointer values need no evaluation. */
    gdb_ensure_disassembly(&g);
    assert(g.instruction_count && g.token == token + 1);

    GdbAddressInfo info;
    assert(!gdb_inspect_address(&g, "cursor", &info));
    assert(info.pointer && !strcmp(info.points_to, "0"));
    token = g.token;
    assert(!gdb_inspect_address(&g, "cursor", &info) && g.token == token);
    char address[128], actual[GD_TEXT_MAX];
    assert(!gdb_expression_address(&g, "dev->status", address, sizeof(address)));
    assert(!strcmp(address, info.address));
    token = g.token;
    assert(!gdb_expression_address(&g, "dev->status", address, sizeof(address)));
    assert(g.token == token);
    assert(!gdb_assign_expression(&g, "dev->status", "44", actual, sizeof(actual)));
    token = g.token;
    assert(!gdb_inspect_address(&g, "cursor", &info) && g.token > token);
    assert(!strcmp(info.points_to, "44"));
    assert(!gdb_print(&g, "dev->status = 55", actual, sizeof(actual)));
    assert(!gdb_inspect_address(&g, "cursor", &info) && !strcmp(info.points_to, "55"));
    assert(!gdb_print(&g, "value = 42", actual, sizeof(actual)));
    assert(condition_candidates(&g, NULL, 0, candidates) == 3);
    int found = 0;
    for (int i = 0; i < 3; i++)
        if (!strcmp(candidates[i].expression, "value") && !strcmp(candidates[i].value, "42"))
            found++;
    assert(found == 1);
    for (int frame = 0; frame < 3; frame++) {
        assert(!gdb_select_frame(&g, frame));
        token = g.token;
        assert(!gdb_inspect_address(&g, frame == 2 ? "dev" : "cursor", &info));
        assert(g.token > token);
    }

    snprintf(functions.input, sizeof(functions.input), "history_");
    refresh_function_dialog(&g, &functions);
    assert(functions.complete && functions.count == 1);
    token = g.token;
    snprintf(functions.input, sizeof(functions.input), "history_record");
    refresh_function_dialog(&g, &functions);
    assert(g.token == token && functions.count == 1);
    unsigned long revision = g.symbol_revision;
    snprintf(g.readbuf, g.readcap, "=library-loaded,id=\"test.so\"\n");
    g.readlen = strlen(g.readbuf);
    gdb_poll(&g, 0);
    assert(g.symbol_revision != revision);
    token = g.token;
    refresh_function_dialog(&g, &functions);
    assert(g.token > token && functions.count == 1);

    char directory[] = "/tmp/gd-cache-test-XXXXXX";
    assert(mkdtemp(directory));
    char path[512];
    snprintf(path, sizeof(path), "%s/one.c", directory);
    write_source(path, "int one;\n");
    load_file_dialog(&g, directory, &files);
    assert(files.all_count == 1 && files.catalogue_valid);
    token = g.token;
    load_file_dialog(&g, directory, &files);
    assert(files.all_count == 1 && g.token == token);
    Source source = {0};
    assert(!source_load(&source, path));
    write_source(path, "int two;\n"); /* Same size, possibly same whole-second mtime. */
    assert(!source_load(&source, path) && strstr(source.lines[0], "two"));
    source_free(&source);
    char second[512];
    snprintf(second, sizeof(second), "%s/two.c", directory);
    write_source(second, "int three;\n");
    load_file_dialog(&g, directory, &files);
    assert(files.all_count == 2);
    assert(!unlink(second));
    load_file_dialog(&g, directory, &files);
    assert(files.all_count == 1);
    assert(!unlink(path) && !rmdir(directory));
    gdb_shutdown(&g);

    memset(&functions, 0, sizeof(functions));
    assert(!gdb_start(&g, argv[3], NULL));
    snprintf(functions.input, sizeof(functions.input), "cache_fn_");
    refresh_function_dialog(&g, &functions);
    assert(functions.count == GD_MAX_FUNCTIONS && !functions.complete);
    token = g.token;
    snprintf(functions.input, sizeof(functions.input), "cache_fn_269");
    refresh_function_dialog(&g, &functions);
    assert(g.token > token && functions.count == 1);
    assert(!strcmp(functions.candidates[0].name, "cache_fn_269"));
    assert(!gdb_cacheable_expression("cursor++"));
    assert(!gdb_cacheable_expression("next_cursor()"));
    assert(!gdb_cacheable_expression("(*function)()"));
    assert(gdb_cacheable_expression("(dev)->status"));
    char *large_output = malloc(100000);
    assert(large_output);
    memset(large_output, 'x', 99999);
    large_output[99999] = 0;
    append_output(&g, large_output);
    assert(g.output_len == sizeof(g.output) - 1);
    assert(g.output[g.output_len] == 0);
    free(large_output);
    gdb_shutdown(&g);
    puts("cache invalidation / candidate completeness / lazy refresh: ok");
    puts("MI requests: Source frame switch=5; fresh B candidates=0; repeated a/F/file menu=0");
    return 0;
}
