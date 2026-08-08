#include <stdio.h>
static void matmul(const int a[2][2], const int b[2][2], int r[2][2]) {
    for (int i = 0; i < 2; ++i)
        for (int j = 0; j < 2; ++j) {
            r[i][j] = 0;
            for (int k = 0; k < 2; ++k) r[i][j] += a[i][k]*b[k][j];
        }
}
int main(void) {
    int a[2][2]={{1,2},{3,4}}, b[2][2]={{2,0},{1,2}}, r[2][2];
    matmul(a,b,r);
    printf("%d\n", r[0][0]);
    return 0;
}
