int main(void) {
    int a[12];
    for (int k = 0; k < 12; ++k) a[k] = k + 0;
    int s = 0;
    for (int k = 1; k < 11; ++k) s += (a[k - 1] + a[k] + a[k + 1]) / 3;
    return s & 255;
}
