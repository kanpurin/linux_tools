struct Device {
    int status;
    int history[4];
};

static void leaf(int *cursor) {
    (*cursor)++;
}

static void history_record(struct Device *dev, int value) {
    int *cursor;
    cursor = &dev->status;
    dev->history[0] = value;
    leaf(cursor);
}

static void process(struct Device *dev, int value) {
    int adjusted = value + 1;
    history_record(dev, adjusted);
}

int main(void) {
    struct Device dev = {0, {0, 0, 0, 0}};
    process(&dev, 10);
    return dev.status == 1 && dev.history[0] == 11 ? 0 : 1;
}
