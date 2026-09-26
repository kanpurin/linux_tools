#include "message.h"
#include "output.h"

#include <stdio.h>

int message_emit(const char *name) {
    char message[256];
    int length = snprintf(message, sizeof(message),
                          "hello %s, this write came through two shared libraries\n",
                          name);
    if (length < 0 || (size_t)length >= sizeof(message)) return -1;
    return output_write_file("/tmp/strace-src-shared-library-demo.txt", message);
}
