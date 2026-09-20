int helper(int value);

int main(void) {
    int value = 3;
    value = helper(value);
    return value == 7 ? 0 : 1;
}
