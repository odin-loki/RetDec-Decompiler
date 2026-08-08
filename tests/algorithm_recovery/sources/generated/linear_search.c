#include <stdio.h>
static int linear_search(const int* a, int n, int x) {
    for (int i = 0; i < n; ++i) if (a[i] == x) return i;
    return -1;
}
int main(void) {
    int a[] = {2,4,6,8,10};
    printf("%d\n", linear_search(a, 5, 6));
    return 0;
}
