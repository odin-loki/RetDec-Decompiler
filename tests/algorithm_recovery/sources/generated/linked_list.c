#include <stdio.h>
#include <stdlib.h>
typedef struct Node { int v; struct Node* next; } Node;
static void list_push(Node** h, int v) {
    Node* n = (Node*)malloc(sizeof(Node));
    n->v = v; n->next = *h; *h = n;
}
int main(void) {
    Node* h = 0;
    list_push(&h, 1); list_push(&h, 2);
    printf("%d\n", h->v);
    return 0;
}
