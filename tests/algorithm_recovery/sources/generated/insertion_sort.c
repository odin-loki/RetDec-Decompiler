#include <stdio.h>
static void insertion_sort(int* a, int n) {
    for (int i = 1; i < n; ++i) {
        int k = a[i], j = i - 1;
        while (j >= 0 && a[j] > k) { a[j+1] = a[j]; --j; }
        a[j+1] = k;
    }
}
int main(void) {
    int a[] = {5,2,4,6,1,3};
    insertion_sort(a, 6);
    for (int i = 0; i < 6; ++i) printf("%d ", a[i]);
    printf("\n");
    return 0;
}
