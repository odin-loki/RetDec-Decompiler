#include <stdio.h>

int binary_search(const int *arr, int n, int target) {
    int lo = 0, hi = n - 1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        if (arr[mid] == target) return mid;
        if (arr[mid] < target) lo = mid + 1;
        else hi = mid - 1;
    }
    return -1;
}

int main(void) {
    int arr[] = {1, 3, 5, 7, 11, 13, 17, 19, 23, 29};
    int n = sizeof(arr) / sizeof(arr[0]);
    int targets[] = {7, 10, 29, 1, 30};
    for (int i = 0; i < 5; i++)
        printf("search(%d) = %d\n", targets[i], binary_search(arr, n, targets[i]));
    return 0;
}
