#include <stdio.h>

__attribute__((noinline)) static int calculate_amount(int base, int extra) {
    return base + extra;
}

__attribute__((noinline)) static int process_payment(int balance, int amount) {
    int total = calculate_amount(amount, 7);
    return balance - total;
}

int main(void) {
    int result = process_payment(1000, 200);
    printf("%d\n", result);
    return result == 793 ? 0 : 1;
}
