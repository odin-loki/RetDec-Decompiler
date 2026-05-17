/* Exercises LLVM intrinsics: memcpy, memmove, memset, memcmp, strlen */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

void *my_memcpy(void *dst, const void *src, size_t n) {
    return memcpy(dst, src, n);
}

void *my_memmove(void *dst, const void *src, size_t n) {
    return memmove(dst, src, n);
}

int my_memcmp(const void *a, const void *b, size_t n) {
    return memcmp(a, b, n);
}

int merge_sort_demo(int *a, int n) {
    if (n <= 1) return 0;
    int *tmp = malloc(n * sizeof(int));
    int m = n / 2;
    merge_sort_demo(a, m);
    merge_sort_demo(a + m, n - m);
    /* merge with memmove */
    memcpy(tmp, a, n * sizeof(int));
    int i = 0, j = m, k = 0;
    while (i < m && j < n) {
        if (tmp[i] <= tmp[j]) a[k++] = tmp[i++];
        else a[k++] = tmp[j++];
    }
    memmove(a + k, tmp + i, (m - i) * sizeof(int));
    memmove(a + k + (m - i), tmp + j, (n - j) * sizeof(int));
    free(tmp);
    return n;
}

int main(void) {
    char src[32] = "Hello from memcpy!";
    char dst[32];
    my_memcpy(dst, src, sizeof(src));
    printf("%s\n", dst);
    
    char buf[32] = "abcdefghij";
    my_memmove(buf + 2, buf, 8);
    printf("memmove: %s\n", buf);
    
    printf("memcmp: %d\n", my_memcmp("abc", "abd", 3));
    
    int arr[] = {5, 2, 8, 1, 9, 3, 7, 4, 6};
    int n = sizeof(arr) / sizeof(arr[0]);
    merge_sort_demo(arr, n);
    for (int i = 0; i < n; i++) printf("%d ", arr[i]);
    printf("\n");
    return 0;
}
