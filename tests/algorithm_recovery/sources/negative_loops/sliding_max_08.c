int main(void) {
    int a[10];
    for (int k = 0; k < 10; ++k) a[k] = (k * 9 + 8) & 31;
    int m = a[0];
    for (int k = 1; k < 10; ++k) if (a[k] > m) m = a[k];
    return m;
}
