#include <stdio.h>
#include <string.h>

#define N 4

void mat_mul(int a[N][N], int b[N][N], int c[N][N]) {
    memset(c, 0, sizeof(int) * N * N);
    for (int i = 0; i < N; i++)
        for (int j = 0; j < N; j++)
            for (int k = 0; k < N; k++)
                c[i][j] += a[i][k] * b[k][j];
}

void mat_print(int m[N][N]) {
    for (int i = 0; i < N; i++) {
        for (int j = 0; j < N; j++)
            printf("%4d ", m[i][j]);
        printf("\n");
    }
}

int main(void) {
    int a[N][N] = {{1,2,3,4},{5,6,7,8},{9,10,11,12},{13,14,15,16}};
    int b[N][N] = {{16,15,14,13},{12,11,10,9},{8,7,6,5},{4,3,2,1}};
    int c[N][N];
    mat_mul(a, b, c);
    mat_print(c);
    return 0;
}
