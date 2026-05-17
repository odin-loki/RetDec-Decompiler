/* Heavy recursion: exercises recursive_cfg_builder, call_info_obtainer */
#include <stdio.h>

/* Mutual recursion */
int is_even(int n);
int is_odd(int n);

int is_even(int n) {
    if (n == 0) return 1;
    return is_odd(n - 1);
}

int is_odd(int n) {
    if (n == 0) return 0;
    return is_even(n - 1);
}

/* Tower of Hanoi */
void hanoi(int n, char from, char to, char via) {
    if (n == 0) return;
    hanoi(n - 1, from, via, to);
    printf("Move disk %d from %c to %c\n", n, from, to);
    hanoi(n - 1, via, to, from);
}

/* Binary tree traversal via recursion */
typedef struct Node {
    int val;
    struct Node *left, *right;
} Node;

Node *new_node(int val) {
    Node *n = __builtin_malloc(sizeof(Node));
    n->val = val; n->left = n->right = 0;
    return n;
}

Node *insert(Node *root, int val) {
    if (!root) return new_node(val);
    if (val < root->val) root->left = insert(root->left, val);
    else root->right = insert(root->right, val);
    return root;
}

void inorder(Node *root) {
    if (!root) return;
    inorder(root->left);
    printf("%d ", root->val);
    inorder(root->right);
}

int tree_height(Node *root) {
    if (!root) return 0;
    int l = tree_height(root->left);
    int r = tree_height(root->right);
    return 1 + (l > r ? l : r);
}

int main(void) {
    printf("is_even(4)=%d is_odd(5)=%d\n", is_even(4), is_odd(5));
    hanoi(3, 'A', 'C', 'B');
    
    Node *root = 0;
    int vals[] = {5, 3, 7, 1, 4, 6, 8};
    for (int i = 0; i < 7; i++) root = insert(root, vals[i]);
    inorder(root);
    printf("\nheight=%d\n", tree_height(root));
    return 0;
}
