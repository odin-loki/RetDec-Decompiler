#include <stdio.h>
#include <math.h>

double newton_sqrt(double x) {
    if (x < 0) return -1.0;
    double guess = x / 2.0;
    for (int i = 0; i < 50; i++) {
        double next = (guess + x / guess) / 2.0;
        if (next == guess) break;
        guess = next;
    }
    return guess;
}

double poly(double x) {
    return 3.0*x*x*x - 2.0*x*x + x - 5.0;
}

int main(void) {
    for (double x = 0.0; x <= 4.0; x += 0.5)
        printf("sqrt(%.1f)=%.6f poly(%.1f)=%.3f\n", x, newton_sqrt(x), x, poly(x));
    return 0;
}
