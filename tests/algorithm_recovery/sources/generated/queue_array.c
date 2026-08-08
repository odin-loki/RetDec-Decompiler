#include <stdio.h>
#define N 16
static int q[N], head, tail, count;
static void enqueue(int v) {
    if (count < N) { q[tail] = v; tail = (tail+1)%N; ++count; }
}
static int dequeue(void) {
    if (!count) return -1;
    int v = q[head]; head = (head+1)%N; --count; return v;
}
int main(void) {
    enqueue(10); enqueue(20);
    printf("%d\n", dequeue());
    return 0;
}
