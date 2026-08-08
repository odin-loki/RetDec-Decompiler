#include <stdio.h>
static void heapify(int* a, int n, int i) {
    int largest = i, l = 2*i+1, r = 2*i+2;
    if (l < n && a[l] > a[largest]) largest = l;
    if (r < n && a[r] > a[largest]) largest = r;
    if (largest != i) {
        int t = a[i]; a[i] = a[largest]; a[largest] = t;
        heapify(a, n, largest);
    }
}
static void heapsort(int* a, int n) {
    for (int i = n/2-1; i >= 0; --i) heapify(a, n, i);
    for (int i = n-1; i > 0; --i) {
        int t = a[0]; a[0] = a[i]; a[i] = t;
        heapify(a, i, 0);
    }
}
int main(void) {
    int a[] = {4,10,3,5,1};
    heapsort(a, 5);
    for (int i = 0; i < 5; ++i) printf("%d ", a[i]);
    printf("\n");
    return 0;
}
