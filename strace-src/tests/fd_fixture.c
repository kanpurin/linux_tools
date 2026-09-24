#include <fcntl.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

static void consume(int fd) {
    char byte;
    (void)read(fd, &byte, 1);
}

int main(void) {
    int original = open("/etc/hostname", O_RDONLY);
    int duplicate;
    pid_t child;
    int reused;
    if (original < 0) return 1;
    duplicate = dup(original);
    if (duplicate < 0) return 1;
    consume(duplicate);
    child = fork();
    if (child == 0) {
        consume(duplicate);
        _exit(0);
    }
    if (child > 0) waitpid(child, NULL, 0);
    close(original);
    reused = open("/etc/passwd", O_RDONLY);
    if (reused >= 0) {
        consume(reused);
        close(reused);
    }
    close(duplicate);
    return 0;
}
