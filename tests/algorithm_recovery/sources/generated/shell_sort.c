#include <stdio.h>
static void shell_sort(int* a, int n) {
    for (int gap = n/2; gap > 0; gap /= 2)
        for (int i = gap; i < n; ++i) {
            int t = a[i], j = i;
            while (j >= gap && a[j-gap] > t) { a[j] = a[j-gap]; j -= gap; }
            a[j] = t;
        }
}
int main(void) {
    int a[] = {9,8,3,7,5,4};
    shell_sort(a, 6);
    for (int i = 0; i < 6; ++i) printf("%d ", a[i]);
    printf("\n");
    return 0;
}
