#include <stdio.h>

volatile int watched;

static int bump(int n) {
    return n + 1;
}

int main(void) {
    int value = 1;
    watched = value;
    value = bump(value);
    watched = value;
    printf("value=%d\n", value);
    return value == 2 ? 0 : 1;
}
