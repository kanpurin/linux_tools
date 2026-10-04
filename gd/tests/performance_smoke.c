#include "../src/gdb.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv) {
    if (argc != 2)
        return 2;
    static Gdb g;
    assert(!gdb_start(&g, argv[1], NULL));
    assert(!gdb_set_function_breakpoint(&g, "main"));
    assert(!gdb_run(&g));
    for (int i = 0; i < 200 && g.state != GDB_STOPPED; i++)
        assert(gdb_poll(&g, 50) >= 0);
    assert(g.state == GDB_STOPPED && g.register_names_valid);
    int token = g.token;
    assert(!gdb_refresh(&g));
    assert(g.token - token == 7);
    char actual[GD_TEXT_MAX];
    assert(!gdb_assign_register(&g, "rax", "0x123", actual, sizeof(actual)));
    int found = 0;
    for (int i = 0; i < g.register_count; i++)
        if (!strcmp(g.registers[i].name, "rax") && strstr(g.registers[i].value, "123"))
            found = 1;
    assert(found); /* Cache names, never values. */

    /* Lifecycle notifications invalidate a target's register number map. */
    const char *events[] = {"=thread-group-started,id=\"i1\",pid=\"123\"\n",
                            "=thread-group-exited,id=\"i1\"\n"};
    for (int i = 0; i < 2; i++) {
        snprintf(g.readbuf, g.readcap, "%s", events[i]);
        g.readlen = strlen(g.readbuf);
        g.changed = false;
        assert(gdb_poll(&g, 0) == 1 && !g.register_names_valid);
        token = g.token;
        assert(!gdb_refresh(&g) && g.register_names_valid);
        assert(g.token - token == 8);
    }
    snprintf(g.readbuf, g.readcap,
             "*stopped,reason=\"exec\",frame={level=\"0\"},thread-id=\"1\"\n");
    g.readlen = strlen(g.readbuf);
    g.changed = false;
    token = g.token;
    assert(gdb_poll(&g, 0) == 1 && g.register_names_valid);
    assert(g.token - token == 8); /* Exec must re-query names before fetching values. */
    gdb_shutdown(&g);
    puts("performance/cache freshness smoke: ok");
    return 0;
}
