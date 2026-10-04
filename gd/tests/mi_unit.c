#include "../src/mi.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    char value[128], quoted[256];
    assert(mi_string("name=\"cursor\",value=\"line\\n\\t\\\"quoted\\\"\\\\end\"", "value", value,
                     sizeof(value)));
    assert(!strcmp(value, "line\n\t\"quoted\"\\end"));
    assert(mi_int("level=\"2\"", "level", -1) == 2);
    assert(mi_int("name=\"cursor\"", "level", -1) == -1);
    assert(!mi_string("name=\"cursor\"", "missing", value, sizeof(value)));
    assert(mi_string("name=\"cursor\"", "name", value, 4));
    assert(!strcmp(value, "cur"));
    char sentinel = 'x';
    assert(!mi_string("name=\"cursor\"", "name", &sentinel, 0));
    quote_mi("cursor", &sentinel, 0);
    assert(sentinel == 'x');
    quote_mi("line\n\t\"quoted\"\\end", quoted, sizeof(quoted));
    char record[512];
    snprintf(record, sizeof(record), "value=%s", quoted);
    assert(mi_string(record, "value", value, sizeof(value)));
    assert(!strcmp(value, "line\n\t\"quoted\"\\end"));

    /* stack-args -> frame -> args; delimiters inside strings are not containers. */
    const char *nested = "[frame={level=\"1\",args=[{name=\"dev\",value=\"{}[]\\\"x\"},"
                         "{name=\"value\",value=\"10\"}]}],tail=\"ok\"";
    const char *list_end = mi_list_end(nested);
    assert(list_end && !strcmp(list_end + 1, ",tail=\"ok\""));
    const char *frame = strchr(nested, '{');
    const char *frame_end = mi_record_end(frame);
    assert(frame_end && frame_end[1] == ']');
    assert(mi_list_end("[frame={args=[]}") == NULL);
    assert(mi_record_end("{name=\"unfinished\\\"}") == NULL);
    puts("MI unit: ok");
    return 0;
}
