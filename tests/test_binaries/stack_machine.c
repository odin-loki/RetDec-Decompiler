/* Simple stack-based expression evaluator */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_STACK 64

typedef struct {
    double data[MAX_STACK];
    int top;
} Stack;

void push(Stack *s, double v) { s->data[s->top++] = v; }
double pop(Stack *s)          { return s->data[--s->top]; }
int empty(Stack *s)           { return s->top == 0; }

double evaluate(const char *tokens[], int n) {
    Stack s = {.top = 0};
    for (int i = 0; i < n; i++) {
        const char *t = tokens[i];
        if (strcmp(t, "+") == 0) { double b = pop(&s), a = pop(&s); push(&s, a+b); }
        else if (strcmp(t, "-") == 0) { double b = pop(&s), a = pop(&s); push(&s, a-b); }
        else if (strcmp(t, "*") == 0) { double b = pop(&s), a = pop(&s); push(&s, a*b); }
        else if (strcmp(t, "/") == 0) { double b = pop(&s), a = pop(&s); push(&s, a/b); }
        else push(&s, atof(t));
    }
    return pop(&s);
}

int main(void) {
    /* (3 + 4) * 2 = 14 */
    const char *e1[] = {"3", "4", "+", "2", "*"};
    printf("%.0f\n", evaluate(e1, 5));
    /* 10 2 / 3 - = 2 */
    const char *e2[] = {"10", "2", "/", "3", "-"};
    printf("%.0f\n", evaluate(e2, 5));
    return 0;
}
