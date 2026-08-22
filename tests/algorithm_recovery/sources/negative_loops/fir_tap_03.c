int main(void) {
    int x[11];
    int c[11];
    int acc = 0;
    for (int k = 0; k < 11; ++k) {
        x[k] = k * 3 + 3;
        c[k] = 1 + (k & 3);
    }
    for (int k = 0; k < 11; ++k) acc += x[k] * c[k];
    return acc & 255;
}
