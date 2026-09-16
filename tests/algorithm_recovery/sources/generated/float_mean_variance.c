#include <stdio.h>
static double mean(const double* v, int n)
{
	double s = 0.0;
	for (int i = 0; i < n; ++i)
		s += v[i];
	return s / (double)n;
}
static double variance(const double* v, int n)
{
	double m = mean(v, n);
	double s = 0.0;
	for (int i = 0; i < n; ++i)
	{
		double d = v[i] - m;
		s += d * d;
	}
	return s / (double)n;
}
int main(void)
{
	double v[5] = {2.0, 4.0, 4.0, 4.0, 5.0};
	printf("%d %d\n", (int)(mean(v, 5) * 10.0), (int)(variance(v, 5) * 10.0));
	return 0;
}
