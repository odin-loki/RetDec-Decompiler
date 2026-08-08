#include <stdio.h>
static int lower_bound(const int* a, int n, int x) {
    int lo = 0, hi = n;
    while (lo < hi) {
        int mid = lo + (hi - lo) / 2;
        if (a[mid] < x) lo = mid + 1; else hi = mid;
    }
    return lo;
}
int main(void) {
    int a[] = {1,2,2,3,4};
    printf("%d\n", lower_bound(a, 5, 2));
    return 0;
}
