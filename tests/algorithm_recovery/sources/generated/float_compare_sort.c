#include <stdio.h>
// Floating-point comparison, which is a different instruction and a different
// flag path from the integer compare every other sort in this corpus uses.
static void insertion_sort_d(double* a, int n) {
    for (int i = 1; i < n; ++i) {
        double key = a[i];
        int j = i - 1;
        while (j >= 0 && a[j] > key) {
            a[j + 1] = a[j];
            --j;
        }
        a[j + 1] = key;
    }
}
int main(void) {
    double a[6] = {3.5, -1.25, 0.0, 9.75, 2.5, -8.0};
    insertion_sort_d(a, 6);
    printf("%d %d\n", (int)(a[0] * 100.0), (int)(a[5] * 100.0));
    return 0;
}
