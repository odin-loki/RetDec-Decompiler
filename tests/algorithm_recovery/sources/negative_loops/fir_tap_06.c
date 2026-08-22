int main(void) {
    int x[14];
    int c[14];
    int acc = 0;
    for (int k = 0; k < 14; ++k) {
        x[k] = k * 3 + 6;
        c[k] = 1 + (k & 3);
    }
    for (int k = 0; k < 14; ++k) acc += x[k] * c[k];
    return acc & 255;
}
