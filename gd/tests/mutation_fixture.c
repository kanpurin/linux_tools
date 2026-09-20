struct Device { int status; };

__attribute__((noinline)) int mutation_worker(struct Device *dev, int value) {
    int local = value;
    dev->status += local;
    return dev->status;
}

__attribute__((noinline)) void mutation_void(struct Device *dev) {
    dev->status = 99;
}

int main(void) {
    struct Device dev = { 1 };
    mutation_void(&dev);
    int result = mutation_worker(&dev, 2);
    return result == 101 ? 0 : 1;
}
