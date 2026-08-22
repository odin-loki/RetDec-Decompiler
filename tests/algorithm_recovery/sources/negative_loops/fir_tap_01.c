int main(void) {
    int x[9];
    int c[9];
    int acc = 0;
    for (int k = 0; k < 9; ++k) {
        x[k] = k * 3 + 1;
        c[k] = 1 + (k & 3);
    }
    for (int k = 0; k < 9; ++k) acc += x[k] * c[k];
    return acc & 255;
}
