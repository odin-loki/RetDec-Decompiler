#include <stdio.h>
// Every direction of the int/float conversion instructions: signed and
// unsigned, both widths, both ways.
static double roundtrip(int v) {
    double d = (double)v;
    float f = (float)d;
    long long l = (long long)(f * 2.0f);
    unsigned u = (unsigned)(d < 0.0 ? -d : d);
    return (double)l + (double)u;
}
int main(void) {
    printf("%d\n", (int)roundtrip(-1234));
    return 0;
}
