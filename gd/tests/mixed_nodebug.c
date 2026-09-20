__attribute__((noinline)) int mixed_nodebug_worker(int value) {
    int adjusted = value + 7;
    return adjusted;
}
