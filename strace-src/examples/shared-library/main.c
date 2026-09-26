#include "message.h"

#include <stdio.h>

int main(int argc, char **argv) {
    const char *name = argc > 1 ? argv[1] : "shared-object";
    if (message_emit(name) < 0) {
        perror("message_emit");
        return 1;
    }
    puts("wrote /tmp/strace-src-shared-library-demo.txt");
    return 0;
}
