/* Algorithm recovery corpus: binary search */
#include <stdio.h>

static int binary_search(const int* a, int n, int target) {
    int lo = 0, hi = n - 1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        if (a[mid] == target) return mid;
        if (a[mid] < target) lo = mid + 1;
        else hi = mid - 1;
    }
    return -1;
}

int main(void) {
    int sorted[] = {1, 3, 5, 7, 9, 11};
    printf("%d\n", binary_search(sorted, 6, 7));
    return 0;
}
