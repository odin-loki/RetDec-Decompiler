int main(void) {
    int x[16];
    int c[16];
    int acc = 0;
    for (int k = 0; k < 16; ++k) {
        x[k] = k * 3 + 8;
        c[k] = 1 + (k & 3);
    }
    for (int k = 0; k < 16; ++k) acc += x[k] * c[k];
    return acc & 255;
}
