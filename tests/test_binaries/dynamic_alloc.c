/* Dynamic allocation patterns: exercises struct recovery, pointer analysis */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Dynamic array (vector) */
typedef struct {
    int *data;
    int size, capacity;
} Vector;

Vector *vec_new(void) {
    Vector *v = malloc(sizeof(Vector));
    v->data = malloc(4 * sizeof(int));
    v->size = 0; v->capacity = 4;
    return v;
}

void vec_push(Vector *v, int val) {
    if (v->size == v->capacity) {
        v->capacity *= 2;
        v->data = realloc(v->data, v->capacity * sizeof(int));
    }
    v->data[v->size++] = val;
}

int vec_get(Vector *v, int i) { return v->data[i]; }

void vec_free(Vector *v) { free(v->data); free(v); }

/* Generic doubly-linked list */
typedef struct DNode {
    int val;
    struct DNode *prev, *next;
} DNode;

typedef struct {
    DNode *head, *tail;
    int count;
} DList;

DList *dlist_new(void) {
    DList *l = calloc(1, sizeof(DList));
    return l;
}

void dlist_push_back(DList *l, int v) {
    DNode *n = malloc(sizeof(DNode));
    n->val = v; n->next = NULL; n->prev = l->tail;
    if (l->tail) l->tail->next = n;
    else l->head = n;
    l->tail = n;
    l->count++;
}

void dlist_push_front(DList *l, int v) {
    DNode *n = malloc(sizeof(DNode));
    n->val = v; n->prev = NULL; n->next = l->head;
    if (l->head) l->head->prev = n;
    else l->tail = n;
    l->head = n;
    l->count++;
}

int dlist_pop_front(DList *l) {
    if (!l->head) return -1;
    DNode *n = l->head;
    int v = n->val;
    l->head = n->next;
    if (l->head) l->head->prev = NULL;
    else l->tail = NULL;
    free(n);
    l->count--;
    return v;
}

int main(void) {
    Vector *v = vec_new();
    for (int i = 0; i < 16; i++) vec_push(v, i * i);
    printf("vec[7]=%d size=%d cap=%d\n", vec_get(v, 7), v->size, v->capacity);
    vec_free(v);
    
    DList *l = dlist_new();
    for (int i = 0; i < 5; i++) dlist_push_back(l, i);
    dlist_push_front(l, 99);
    printf("count=%d front=%d\n", l->count, dlist_pop_front(l));
    for (DNode *n = l->head; n; n = n->next)
        printf("%d ", n->val);
    printf("\n");
    return 0;
}
