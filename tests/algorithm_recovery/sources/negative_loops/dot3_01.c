int main(void) {
    int a[3] = {1, 2, 4};
    int b[3] = {1, 4, 5};
    int d = 0;
    for (int k = 0; k < 3; ++k) d += a[k] * b[k];
    return d & 255;
}
