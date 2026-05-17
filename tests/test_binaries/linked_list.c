#include <stdlib.h>
#include <stdio.h>
typedef struct Node { int val; struct Node* next; } Node;
Node* push(Node* head, int val) {
    Node* n = (Node*)malloc(sizeof(Node));
    n->val = val; n->next = head; return n;
}
void print_list(Node* head) {
    while (head) {
        printf("%d -> ", head->val);
        head = head->next;
    }
    printf("NULL\n");
}
int main() {
    Node* list = NULL;
    int i;
    for (i = 1; i <= 5; i++) list = push(list, i*10);
    print_list(list);
    return 0;
}
