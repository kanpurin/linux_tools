#include "output.h"

#include <fcntl.h>
#include <stddef.h>
#include <string.h>
#include <unistd.h>

int output_write_file(const char *path, const char *message) {
    size_t length = strlen(message);
    size_t written = 0;
    int fd = open(path, O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (fd < 0) return -1;

    while (written < length) {
        ssize_t result = write(fd, message + written, length - written);
        if (result < 0) {
            close(fd);
            return -1;
        }
        written += (size_t)result;
    }

    if (close(fd) < 0) return -1;
    return 0;
}
