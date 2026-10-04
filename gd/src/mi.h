#ifndef GD_MI_H
#define GD_MI_H

#include <stdbool.h>
#include <stddef.h>

/* MI string fields and balanced containers. No GDB process or UI dependency. */
bool mi_string(const char *line, const char *key, char *out, size_t size);
int mi_int(const char *line, const char *key, int fallback);
void quote_mi(const char *src, char *dst, size_t size);
const char *mi_record_end(const char *start);
const char *mi_list_end(const char *start);

#endif
