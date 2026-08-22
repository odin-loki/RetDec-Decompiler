/* 1-based heapsort. a[0] is an unused sentinel, not 0-based 2*i+1 children. */
#include <stdio.h>

static void sift(int* a, int n, int i)
{
	for (;;)
	{
		int l = i * 2;
		int r = l + 1;
		int m = i;
		if (l <= n && a[l] > a[m]) m = l;
		if (r <= n && a[r] > a[m]) m = r;
		if (m == i) break;
		int t = a[i];
		a[i] = a[m];
		a[m] = t;
		i = m;
	}
}

static void heapsort_sentinel(int* a, int n)
{
	for (int i = n / 2; i >= 1; --i) sift(a, n, i);
	for (int i = n; i > 1; --i)
	{
		int t = a[1];
		a[1] = a[i];
		a[i] = t;
		sift(a, i - 1, 1);
	}
}

int main(void)
{
	int a[] = {0, 4, 10, 3, 5, 1};
	heapsort_sentinel(a, 5);
	for (int i = 1; i <= 5; ++i) printf("%d ", a[i]);
	printf("\n");
	return 0;
}
