#include <stdio.h>
#define N 4
static void matmul(const float a[N][N], const float b[N][N], float c[N][N])
{
	for (int i = 0; i < N; ++i)
		for (int j = 0; j < N; ++j)
		{
			float s = 0.0f;
			for (int k = 0; k < N; ++k)
				s += a[i][k] * b[k][j];
			c[i][j] = s;
		}
}
int main(void)
{
	float a[N][N], b[N][N], c[N][N];
	for (int i = 0; i < N; ++i)
		for (int j = 0; j < N; ++j)
		{
			a[i][j] = (float)(i + j);
			b[i][j] = (float)(i - j);
		}
	matmul(a, b, c);
	printf("%d\n", (int)c[N - 1][N - 1]);
	return 0;
}
