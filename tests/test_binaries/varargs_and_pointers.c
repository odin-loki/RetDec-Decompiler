/* Variadic functions, function pointers: exercises call analysis */
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>

/* Variadic sum */
double vsum(int count, ...) {
    va_list ap;
    va_start(ap, count);
    double total = 0.0;
    for (int i = 0; i < count; i++)
        total += va_arg(ap, double);
    va_end(ap);
    return total;
}

/* Function pointers */
typedef int (*compare_fn)(const void *, const void *);

int cmp_int_asc(const void *a, const void *b)  { return *(int*)a - *(int*)b; }
int cmp_int_desc(const void *a, const void *b) { return *(int*)b - *(int*)a; }

void sort_with(int *arr, int n, compare_fn fn) {
    qsort(arr, n, sizeof(int), fn);
}

/* Array of function pointers */
typedef double (*math_fn)(double);

double sq(double x)   { return x * x; }
double cube(double x) { return x * x * x; }
double recip(double x){ return x != 0.0 ? 1.0 / x : 0.0; }

void apply_all(double x, math_fn fns[], int n) {
    for (int i = 0; i < n; i++)
        printf("  fn[%d](%.1f) = %.3f\n", i, x, fns[i](x));
}

/* Callback pattern */
void for_each(int *arr, int n, void (*fn)(int)) {
    for (int i = 0; i < n; i++) fn(arr[i]);
}

void print_int(int x) { printf("%d ", x); }

int main(void) {
    printf("vsum(3) = %.1f\n", vsum(3, 1.0, 2.0, 3.0));
    printf("vsum(5) = %.1f\n", vsum(5, 1.0, 2.0, 3.0, 4.0, 5.0));
    
    int arr[] = {5, 2, 8, 1, 9};
    int n = 5;
    sort_with(arr, n, cmp_int_asc);
    for_each(arr, n, print_int); printf("\n");
    sort_with(arr, n, cmp_int_desc);
    for_each(arr, n, print_int); printf("\n");
    
    math_fn fns[] = {sq, cube, recip};
    apply_all(3.0, fns, 3);
    return 0;
}
