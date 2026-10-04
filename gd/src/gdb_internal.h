#ifndef GD_GDB_INTERNAL_H
#define GD_GDB_INTERNAL_H

#include "gdb.h"

/* Private backend helpers. Public operations remain in gdb.h. */
void copy_text(char *dst, size_t size, const char *src);
const char *gtext(const Gdb *g, const char *english, const char *japanese);
void append_output(Gdb *g, const char *text);
int request(Gdb *g, char *result, size_t size, const char *fmt, ...);

#endif
