int main(void) {
    int x[12];
    int c[12];
    int acc = 0;
    for (int k = 0; k < 12; ++k) {
        x[k] = k * 3 + 4;
        c[k] = 1 + (k & 3);
    }
    for (int k = 0; k < 12; ++k) acc += x[k] * c[k];
    return acc & 255;
}
