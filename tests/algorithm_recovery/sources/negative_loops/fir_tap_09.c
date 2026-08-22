int main(void) {
    int x[17];
    int c[17];
    int acc = 0;
    for (int k = 0; k < 17; ++k) {
        x[k] = k * 3 + 9;
        c[k] = 1 + (k & 3);
    }
    for (int k = 0; k < 17; ++k) acc += x[k] * c[k];
    return acc & 255;
}
