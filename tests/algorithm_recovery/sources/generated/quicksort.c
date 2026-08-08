#include <stdio.h>
static void swap(int* a, int* b) { int t = *a; *a = *b; *b = t; }
static int partition(int* a, int lo, int hi) {
    int pivot = a[hi], i = lo - 1;
    for (int j = lo; j < hi; ++j)
        if (a[j] <= pivot) { ++i; swap(&a[i], &a[j]); }
    swap(&a[i + 1], &a[hi]);
    return i + 1;
}
static void quicksort(int* a, int lo, int hi) {
    if (lo < hi) {
        int p = partition(a, lo, hi);
        quicksort(a, lo, p - 1);
        quicksort(a, p + 1, hi);
    }
}
int main(void) {
    int a[] = {3,1,4,1,5,9,2,6};
    quicksort(a, 0, 7);
    for (int i = 0; i < 8; ++i) printf("%d ", a[i]);
    printf("\n");
    return 0;
}
