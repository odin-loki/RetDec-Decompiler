/* Global variables, integer structs, enums: exercises global_var_def, for_loop_stmt */
#include <stdio.h>
#include <string.h>

typedef enum { RED, GREEN, BLUE, ALPHA } Channel;
typedef struct { int r, g, b, a; } Color;   /* integer RGBA to avoid SSE */
typedef struct { int x, y, w, h; } Rect;
typedef struct { long long lo, hi; } Range;

static Color palette[4] = {
    {255, 0,   0,   255},
    {0,   255, 0,   255},
    {0,   0,   255, 255},
    {255, 255, 255, 128},
};

static int call_count = 0;
static long long running_total = 0;

Color mix_int(Color a, Color b) {
    Color r;
    r.r = (a.r + b.r) / 2;
    r.g = (a.g + b.g) / 2;
    r.b = (a.b + b.b) / 2;
    r.a = (a.a + b.a) / 2;
    return r;
}

int channel(Color c, Channel ch) {
    switch (ch) {
        case RED:   return c.r;
        case GREEN: return c.g;
        case BLUE:  return c.b;
        case ALPHA: return c.a;
        default:    return 0;
    }
}

int rects_overlap(Rect a, Rect b) {
    return !(a.x + a.w <= b.x || b.x + b.w <= a.x ||
             a.y + a.h <= b.y || b.y + b.h <= a.y);
}

long long accumulate(long long v) {
    call_count++;
    running_total += v;
    return running_total;
}

int range_contains(Range r, long long v) {
    return v >= r.lo && v <= r.hi;
}

int main(void) {
    for (int i = 0; i < 4; i++) {
        Color c = palette[i];
        printf("palette[%d]: r=%d g=%d b=%d a=%d\n", i, c.r, c.g, c.b, c.a);
    }
    Color m = mix_int(palette[0], palette[1]);
    printf("mix: r=%d g=%d b=%d\n", m.r, m.g, m.b);
    printf("channel RED=%d\n", channel(palette[0], RED));
    
    Rect r1 = {0, 0, 10, 10};
    Rect r2 = {5, 5, 10, 10};
    Rect r3 = {20, 20, 5, 5};
    printf("overlap(r1,r2)=%d overlap(r1,r3)=%d\n", rects_overlap(r1,r2), rects_overlap(r1,r3));
    
    for (long long v = 1; v <= 5; v++) accumulate(v);
    printf("total=%lld calls=%d\n", running_total, call_count);
    
    Range rng = {10, 50};
    printf("contains(30)=%d contains(5)=%d\n", range_contains(rng, 30), range_contains(rng, 5));
    return 0;
}
