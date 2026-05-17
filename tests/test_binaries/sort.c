#include <stdio.h>
void bubble_sort(int* a, int n) {
    int i, j, tmp;
    for (i = 0; i < n-1; i++)
        for (j = 0; j < n-i-1; j++)
            if (a[j] > a[j+1]) { tmp=a[j]; a[j]=a[j+1]; a[j+1]=tmp; }
}
void insertion_sort(int* a, int n) {
    int i, j, key;
    for (i = 1; i < n; i++) {
        key = a[i]; j = i - 1;
        while (j >= 0 && a[j] > key) { a[j+1] = a[j]; j--; }
        a[j+1] = key;
    }
}
int main() {
    int arr[] = {64, 34, 25, 12, 22, 11, 90};
    int n = 7, i;
    bubble_sort(arr, n);
    for (i = 0; i < n; i++) printf("%d ", arr[i]);
    printf("\n");
    return 0;
}
