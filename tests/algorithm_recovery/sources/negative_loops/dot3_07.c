int main(void) {
    int a[3] = {1, 2, 10};
    int b[3] = {7, 4, 5};
    int d = 0;
    for (int k = 0; k < 3; ++k) d += a[k] * b[k];
    return d & 255;
}
