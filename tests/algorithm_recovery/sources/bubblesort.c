/* Algorithm recovery corpus: bubble sort */
#include <stdio.h>

static void bubble_sort(int* a, int n) {
    for (int i = 0; i < n - 1; ++i)
        for (int j = 0; j < n - 1 - i; ++j)
            if (a[j] > a[j + 1]) {
                int t = a[j];
                a[j] = a[j + 1];
                a[j + 1] = t;
            }
}

int main(void) {
    int data[] = {5, 3, 8, 1, 9, 2};
    bubble_sort(data, 6);
    for (int i = 0; i < 6; ++i) printf("%d ", data[i]);
    printf("\n");
    return 0;
}
