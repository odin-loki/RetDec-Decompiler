int main(void) {
    int acc = 0;
    for (int k = 0; k < 13; ++k) {
        acc += k * 3;
        if (acc > 200) acc = 200;
    }
    return acc;
}
