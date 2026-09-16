#include <stdio.h>
// No libm: a call to sqrt() would be a PLT stub and would say nothing about
// the translator. This is the square root written in multiplies and divides.
static double newton_sqrt(double x)
{
	if (x <= 0.0) return 0.0;
	double guess = x;
	for (int i = 0; i < 24; ++i)
	{
		double next = 0.5 * (guess + x / guess);
		double diff = next - guess;
		if (diff < 0.0) diff = -diff;
		guess = next;
		if (diff < 1e-12) break;
	}
	return guess;
}
int main(void)
{
	printf("%d\n", (int)(newton_sqrt(2.0) * 1000.0));
	return 0;
}
