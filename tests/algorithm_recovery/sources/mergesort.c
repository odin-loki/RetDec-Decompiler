/* Algorithm recovery corpus: mergesort */
#include <stdio.h>
#include <stdlib.h>

static void merge(int* a, int* tmp, int lo, int mid, int hi) {
    int i = lo, j = mid + 1, k = lo;
    while (i <= mid && j <= hi)
        tmp[k++] = (a[i] <= a[j]) ? a[i++] : a[j++];
    while (i <= mid) tmp[k++] = a[i++];
    while (j <= hi) tmp[k++] = a[j++];
    for (int t = lo; t <= hi; ++t) a[t] = tmp[t];
}

static void mergesort(int* a, int* tmp, int lo, int hi) {
    if (lo >= hi) return;
    int mid = lo + (hi - lo) / 2;
    mergesort(a, tmp, lo, mid);
    mergesort(a, tmp, mid + 1, hi);
    merge(a, tmp, lo, mid, hi);
}

int main(void) {
    int data[] = {9, 4, 7, 1, 3, 8, 2};
    int tmp[7];
    mergesort(data, tmp, 0, 6);
    for (int i = 0; i < 7; ++i) printf("%d ", data[i]);
    printf("\n");
    return 0;
}
