#include "mi.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool mi_string(const char *line, const char *key, char *out, size_t size) {
    if (!size)
        return false;
    char needle[128];
    snprintf(needle, sizeof(needle), "%s=\"", key);
    const char *p = strstr(line, needle);
    if (!p)
        return false;
    p += strlen(needle);
    size_t n = 0;
    while (*p && *p != '"' && n + 1 < size) {
        if (*p == '\\' && p[1]) {
            ++p;
            switch (*p) {
            case 'n':
                out[n++] = '\n';
                break;
            case 'r':
                out[n++] = '\r';
                break;
            case 't':
                out[n++] = '\t';
                break;
            default:
                out[n++] = *p;
                break;
            }
            ++p;
        } else
            out[n++] = *p++;
    }
    out[n] = '\0';
    return true;
}

int mi_int(const char *line, const char *key, int fallback) {
    char value[64];
    if (!mi_string(line, key, value, sizeof(value)))
        return fallback;
    return atoi(value);
}

void quote_mi(const char *src, char *dst, size_t size) {
    if (!size)
        return;
    size_t n = 0;
    if (size)
        dst[n++] = '"';
    for (; *src && n + 2 < size; ++src) {
        if (*src == '\n' || *src == '\r' || *src == '\t') {
            dst[n++] = '\\';
            dst[n++] = *src == '\n' ? 'n' : *src == '\r' ? 'r' : 't';
            continue;
        }
        if (*src == '\\' || *src == '"')
            dst[n++] = '\\';
        dst[n++] = *src;
    }
    if (n + 1 < size)
        dst[n++] = '"';
    dst[n < size ? n : size - 1] = '\0';
}

static const char *mi_container_end(const char *start, char opening, char closing) {
    int depth = 0;
    bool quoted = false, escaped = false;
    for (const char *p = start; *p; ++p) {
        if (escaped) {
            escaped = false;
            continue;
        }
        if (quoted && *p == '\\') {
            escaped = true;
            continue;
        }
        if (*p == '"') {
            quoted = !quoted;
            continue;
        }
        if (quoted)
            continue;
        if (*p == opening)
            depth++;
        else if (*p == closing && --depth == 0)
            return p;
    }
    return NULL;
}

const char *mi_record_end(const char *start) {
    return mi_container_end(start, '{', '}');
}

const char *mi_list_end(const char *start) {
    return mi_container_end(start, '[', ']');
}
