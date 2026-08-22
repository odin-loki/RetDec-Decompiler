int main(void) {
    int x[8];
    int c[8];
    int acc = 0;
    for (int k = 0; k < 8; ++k) {
        x[k] = k * 3 + 0;
        c[k] = 1 + (k & 3);
    }
    for (int k = 0; k < 8; ++k) acc += x[k] * c[k];
    return acc & 255;
}
