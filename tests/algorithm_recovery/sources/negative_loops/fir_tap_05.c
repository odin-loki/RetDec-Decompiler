int main(void) {
    int x[13];
    int c[13];
    int acc = 0;
    for (int k = 0; k < 13; ++k) {
        x[k] = k * 3 + 5;
        c[k] = 1 + (k & 3);
    }
    for (int k = 0; k < 13; ++k) acc += x[k] * c[k];
    return acc & 255;
}
