#ifndef STRACE_SRC_RESOLVE_CACHE_H
#define STRACE_SRC_RESOLVE_CACHE_H

#include "strace_src.h"

/* Cached strings are copied into the frame: events retain their own lifetime. */
bool resolve_cache_get(StackFrame *frame);
void resolve_cache_put(const StackFrame *frame);
void resolve_cache_clear(void);

#endif
