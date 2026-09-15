#include <stdio.h>
static double dot_product(const double* a, const double* b, int n) {
    double sum = 0.0;
    for (int i = 0; i < n; ++i)
        sum += a[i] * b[i];
    return sum;
}
int main(void) {
    double a[4] = {1.5, 2.5, 3.5, 4.5};
    double b[4] = {2.0, 4.0, 0.5, 1.0};
    printf("%d\n", (int)(dot_product(a, b, 4) * 100.0));
    return 0;
}
