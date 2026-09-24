#include <stdio.h>
#include <unistd.h>

static void say_hello(void) {
    puts("Hello from strace-src fixture");
}

int main(void) {
    say_hello();
    sleep(1);
    return 0;
}
