int main(void) {
    int x[15];
    int c[15];
    int acc = 0;
    for (int k = 0; k < 15; ++k) {
        x[k] = k * 3 + 7;
        c[k] = 1 + (k & 3);
    }
    for (int k = 0; k < 15; ++k) acc += x[k] * c[k];
    return acc & 255;
}
