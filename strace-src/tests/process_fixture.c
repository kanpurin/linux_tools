#define _GNU_SOURCE
#include <fcntl.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static void *worker(void *unused) {
    int fd;
    (void)unused;
    pthread_setname_np(pthread_self(), "worker");
    fd = open("/definitely/missing/strace-src", O_RDONLY);
    if (fd >= 0) close(fd);
    sleep(1);
    return NULL;
}

int main(void) {
    pthread_t thread;
    pid_t child;
    pthread_setname_np(pthread_self(), "app");
    if (pthread_create(&thread, NULL, worker, NULL) != 0) return 1;
    pthread_join(thread, NULL);
    child = fork();
    if (child == 0) {
        execl("/bin/true", "helper", (char *)NULL);
        _exit(127);
    }
    if (child > 0) waitpid(child, NULL, 0);
    return 0;
}
