int main(void) {
    int x[10];
    int c[10];
    int acc = 0;
    for (int k = 0; k < 10; ++k) {
        x[k] = k * 3 + 2;
        c[k] = 1 + (k & 3);
    }
    for (int k = 0; k < 10; ++k) acc += x[k] * c[k];
    return acc & 255;
}
