extern int mixed_nodebug_worker(int value);

int main(void) {
    int value = 5;
    value = mixed_nodebug_worker(value);
    return value == 12 ? 0 : 1;
}
