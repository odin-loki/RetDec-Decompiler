#include <stdio.h>
#define N 16
static int stk[N], top;
static void push(int v) { if (top < N) stk[top++] = v; }
static int pop(void) { return top ? stk[--top] : -1; }
int main(void) {
    push(1); push(2); push(3);
    printf("%d %d\n", pop(), pop());
    return 0;
}
