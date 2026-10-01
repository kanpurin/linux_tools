#include <sys/prctl.h>
#include <unistd.h>

volatile int counter;

int main(void) {
    /* Permit the test's sibling GDB under Linux Yama ptrace_scope=1. */
    prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY, 0, 0, 0);
    for (;;) {
        ++counter;
        if (write(STDOUT_FILENO, ".", 1) != 1) return 1;
        usleep(20000);
    }
}
