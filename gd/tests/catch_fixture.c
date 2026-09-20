#include <fcntl.h>
#include <sys/syscall.h>
#include <unistd.h>

int main(void) {
    int fd = (int)syscall(SYS_openat, AT_FDCWD, "/dev/null", O_RDONLY, 0);
    if (fd >= 0) close(fd);
    return fd < 0;
}
