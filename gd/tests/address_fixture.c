struct Holder {
    int target;
    int other;
};

int main(void) {
    int value = 7;
    int *cursor = &value;
    int *null_ptr = 0;
    int *bad_ptr = (int *)1;
    struct Holder holder = {9, 10};
    struct Holder *holder_ptr = &holder;
    int *member_cursor = &holder.target;
    *cursor += 1;
    return value == 8 && null_ptr == 0 && bad_ptr != 0 &&
           holder_ptr->target == 9 && *member_cursor == 9 ? 0 : 1;
}
