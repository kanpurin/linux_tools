#include <stdio.h>

struct Node {
    int value;
    struct Node *next;
};

int main(void) {
    struct Node tail = { 20, NULL };
    struct Node head = { 10, &tail };
    struct Node *selected = &head;
    selected->value++;
    printf("%d\n", selected->value);
    return 0;
}
