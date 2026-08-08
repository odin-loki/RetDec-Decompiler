#!/usr/bin/env python3
"""Generate algorithm-recovery C sources + label sidecars from catalog (step 10)."""
from __future__ import annotations

import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "tests" / "algorithm_recovery" / "sources" / "generated"

# Each entry: (stem, algorithms[], c_source)
SPECS: list[tuple[str, list[str], str]] = [
    ("quicksort", ["QuickSort", "Sort"], """
#include <stdio.h>
static void swap(int* a, int* b) { int t = *a; *a = *b; *b = t; }
static int partition(int* a, int lo, int hi) {
    int pivot = a[hi], i = lo - 1;
    for (int j = lo; j < hi; ++j)
        if (a[j] <= pivot) { ++i; swap(&a[i], &a[j]); }
    swap(&a[i + 1], &a[hi]);
    return i + 1;
}
static void quicksort(int* a, int lo, int hi) {
    if (lo < hi) {
        int p = partition(a, lo, hi);
        quicksort(a, lo, p - 1);
        quicksort(a, p + 1, hi);
    }
}
int main(void) {
    int a[] = {3,1,4,1,5,9,2,6};
    quicksort(a, 0, 7);
    for (int i = 0; i < 8; ++i) printf("%d ", a[i]);
    printf("\\n");
    return 0;
}
"""),
    ("heapsort", ["HeapSort", "Sort"], """
#include <stdio.h>
static void heapify(int* a, int n, int i) {
    int largest = i, l = 2*i+1, r = 2*i+2;
    if (l < n && a[l] > a[largest]) largest = l;
    if (r < n && a[r] > a[largest]) largest = r;
    if (largest != i) {
        int t = a[i]; a[i] = a[largest]; a[largest] = t;
        heapify(a, n, largest);
    }
}
static void heapsort(int* a, int n) {
    for (int i = n/2-1; i >= 0; --i) heapify(a, n, i);
    for (int i = n-1; i > 0; --i) {
        int t = a[0]; a[0] = a[i]; a[i] = t;
        heapify(a, i, 0);
    }
}
int main(void) {
    int a[] = {4,10,3,5,1};
    heapsort(a, 5);
    for (int i = 0; i < 5; ++i) printf("%d ", a[i]);
    printf("\\n");
    return 0;
}
"""),
    ("insertion_sort", ["InsertionSort", "Sort"], """
#include <stdio.h>
static void insertion_sort(int* a, int n) {
    for (int i = 1; i < n; ++i) {
        int k = a[i], j = i - 1;
        while (j >= 0 && a[j] > k) { a[j+1] = a[j]; --j; }
        a[j+1] = k;
    }
}
int main(void) {
    int a[] = {5,2,4,6,1,3};
    insertion_sort(a, 6);
    for (int i = 0; i < 6; ++i) printf("%d ", a[i]);
    printf("\\n");
    return 0;
}
"""),
    ("selection_sort", ["SelectionSort", "Sort"], """
#include <stdio.h>
static void selection_sort(int* a, int n) {
    for (int i = 0; i < n-1; ++i) {
        int m = i;
        for (int j = i+1; j < n; ++j)
            if (a[j] < a[m]) m = j;
        int t = a[i]; a[i] = a[m]; a[m] = t;
    }
}
int main(void) {
    int a[] = {64,25,12,22,11};
    selection_sort(a, 5);
    for (int i = 0; i < 5; ++i) printf("%d ", a[i]);
    printf("\\n");
    return 0;
}
"""),
    ("shell_sort", ["ShellSort", "Sort"], """
#include <stdio.h>
static void shell_sort(int* a, int n) {
    for (int gap = n/2; gap > 0; gap /= 2)
        for (int i = gap; i < n; ++i) {
            int t = a[i], j = i;
            while (j >= gap && a[j-gap] > t) { a[j] = a[j-gap]; j -= gap; }
            a[j] = t;
        }
}
int main(void) {
    int a[] = {9,8,3,7,5,4};
    shell_sort(a, 6);
    for (int i = 0; i < 6; ++i) printf("%d ", a[i]);
    printf("\\n");
    return 0;
}
"""),
    ("linear_search", ["LinearSearch", "Search"], """
#include <stdio.h>
static int linear_search(const int* a, int n, int x) {
    for (int i = 0; i < n; ++i) if (a[i] == x) return i;
    return -1;
}
int main(void) {
    int a[] = {2,4,6,8,10};
    printf("%d\\n", linear_search(a, 5, 6));
    return 0;
}
"""),
    ("stack_array", ["Stack", "LIFO"], """
#include <stdio.h>
#define N 16
static int stk[N], top;
static void push(int v) { if (top < N) stk[top++] = v; }
static int pop(void) { return top ? stk[--top] : -1; }
int main(void) {
    push(1); push(2); push(3);
    printf("%d %d\\n", pop(), pop());
    return 0;
}
"""),
    ("queue_array", ["Queue", "FIFO"], """
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
    printf("%d\\n", dequeue());
    return 0;
}
"""),
    ("strlen_loop", ["Strlen", "String"], """
#include <stdio.h>
static int my_strlen(const char* s) {
    int n = 0;
    while (s[n]) ++n;
    return n;
}
int main(void) {
    printf("%d\\n", my_strlen("hello"));
    return 0;
}
"""),
    ("strcmp_loop", ["Strcmp", "String"], """
#include <stdio.h>
static int my_strcmp(const char* a, const char* b) {
    while (*a && *a == *b) { ++a; ++b; }
    return (unsigned char)*a - (unsigned char)*b;
}
int main(void) {
    printf("%d\\n", my_strcmp("abc", "abd"));
    return 0;
}
"""),
    ("fibonacci_iter", ["Fibonacci", "DynamicProgramming"], """
#include <stdio.h>
static unsigned fib(unsigned n) {
    unsigned a = 0, b = 1;
    for (unsigned i = 0; i < n; ++i) {
        unsigned t = a + b; a = b; b = t;
    }
    return a;
}
int main(void) { printf("%u\\n", fib(10)); return 0; }
"""),
    ("gcd_euclid", ["GCD", "Euclid"], """
#include <stdio.h>
static int gcd(int a, int b) {
    while (b) { int t = b; b = a % b; a = t; }
    return a;
}
int main(void) { printf("%d\\n", gcd(48, 18)); return 0; }
"""),
    ("xor_cipher", ["XOR", "Cipher"], """
#include <stdio.h>
static void xor_cipher(unsigned char* buf, int n, unsigned char k) {
    for (int i = 0; i < n; ++i) buf[i] ^= k;
}
int main(void) {
    unsigned char msg[] = "data";
    xor_cipher(msg, 4, 0x5A);
    printf("%02x\\n", msg[0]);
    return 0;
}
"""),
    ("dfs_graph", ["DFS", "GraphTraversal"], """
#include <stdio.h>
#define V 4
static int adj[V][V] = {{0,1,1,0},{1,0,0,1},{1,0,0,1},{0,1,1,0}};
static int vis[V];
static void dfs(int u) {
    vis[u] = 1;
    for (int v = 0; v < V; ++v)
        if (adj[u][v] && !vis[v]) dfs(v);
}
int main(void) { dfs(0); printf("%d\\n", vis[3]); return 0; }
"""),
    ("bfs_graph", ["BFS", "GraphTraversal"], """
#include <stdio.h>
#define V 4
static int adj[V][V] = {{0,1,0,1},{1,0,1,0},{0,1,0,1},{1,0,1,0}};
static int vis[V], q[V], head, tail;
static void bfs(int s) {
    vis[s]=1; q[tail++]=s;
    while (head < tail) {
        int u = q[head++];
        for (int v=0; v<V; ++v)
            if (adj[u][v] && !vis[v]) { vis[v]=1; q[tail++]=v; }
    }
}
int main(void) { bfs(0); printf("%d\\n", vis[2]); return 0; }
"""),
    ("crc32_simple", ["CRC", "Checksum"], """
#include <stdio.h>
#include <stdint.h>
static uint32_t crc32_byte(uint32_t crc, uint8_t b) {
    crc ^= b;
    for (int i = 0; i < 8; ++i)
        crc = (crc >> 1) ^ (0xEDB88320u & (-(int)(crc & 1)));
    return crc;
}
int main(void) {
    uint32_t c = 0xFFFFFFFFu;
    c = crc32_byte(c, 'A');
    printf("%08x\\n", ~c);
    return 0;
}
"""),
    ("bitcount", ["Popcount", "BitManipulation"], """
#include <stdio.h>
static int popcount(unsigned x) {
    int c = 0;
    while (x) { c += x & 1; x >>= 1; }
    return c;
}
int main(void) { printf("%d\\n", popcount(0b101101)); return 0; }
"""),
    ("matrix_mul", ["MatrixMultiply", "LinearAlgebra"], """
#include <stdio.h>
static void matmul(const int a[2][2], const int b[2][2], int r[2][2]) {
    for (int i = 0; i < 2; ++i)
        for (int j = 0; j < 2; ++j) {
            r[i][j] = 0;
            for (int k = 0; k < 2; ++k) r[i][j] += a[i][k]*b[k][j];
        }
}
int main(void) {
    int a[2][2]={{1,2},{3,4}}, b[2][2]={{2,0},{1,2}}, r[2][2];
    matmul(a,b,r);
    printf("%d\\n", r[0][0]);
    return 0;
}
"""),
    ("knapsack_01", ["Knapsack", "DynamicProgramming"], """
#include <stdio.h>
static int knapsack(const int w[], const int v[], int n, int cap) {
    int dp[32] = {0};
    for (int i = 0; i < n; ++i)
        for (int c = cap; c >= w[i]; --c)
            if (dp[c-w[i]] + v[i] > dp[c]) dp[c] = dp[c-w[i]] + v[i];
    return dp[cap];
}
int main(void) {
    int w[]={1,3,4}, v[]={15,20,30};
    printf("%d\\n", knapsack(w,v,3,7));
    return 0;
}
"""),
    ("lcs_dp", ["LCS", "DynamicProgramming"], """
#include <stdio.h>
static int lcs(const char* a, const char* b) {
    int dp[16][16] = {0};
    for (int i = 1; a[i-1]; ++i)
        for (int j = 1; b[j-1]; ++j)
            dp[i][j] = (a[i-1]==b[j-1]) ? dp[i-1][j-1]+1 :
                (dp[i-1][j] > dp[i][j-1] ? dp[i-1][j] : dp[i][j-1]);
    int la=0, lb=0; while(a[la])++la; while(b[lb])++lb;
    return dp[la][lb];
}
int main(void) { printf("%d\\n", lcs("abcde", "ace")); return 0; }
"""),
    ("rle_encode", ["RLE", "Compression"], """
#include <stdio.h>
static int rle_encode(const char* in, char* out) {
    int n = 0, o = 0;
    while (in[n]) {
        char c = in[n]; int run = 1;
        while (in[n+run] == c) ++run;
        out[o++] = c; out[o++] = (char)('0' + run);
        n += run;
    }
    out[o] = '\\0';
    return o;
}
int main(void) {
    char out[32];
    rle_encode("aaabbc", out);
    printf("%s\\n", out);
    return 0;
}
"""),
    ("varint_encode", ["Varint", "Serialization"], """
#include <stdio.h>
#include <stdint.h>
static int encode_varint(uint32_t v, unsigned char* out) {
    int i = 0;
    while (v >= 0x80) { out[i++] = (unsigned char)((v & 0x7F) | 0x80); v >>= 7; }
    out[i++] = (unsigned char)v;
    return i;
}
int main(void) {
    unsigned char buf[8];
    int n = encode_varint(300, buf);
    printf("%d %d\\n", n, buf[0]);
    return 0;
}
"""),
    ("linked_list", ["LinkedList"], """
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
    printf("%d\\n", h->v);
    return 0;
}
"""),
    ("separate_chaining", ["HashTable", "Chaining"], """
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define B 8
typedef struct E { char k[8]; int v; struct E* n; } E;
static E* buckets[B];
static unsigned h(const char* s) {
    unsigned x = 0; while (*s) x = x*31 + (unsigned char)*s++;
    return x % B;
}
static void put(const char* k, int v) {
    unsigned i = h(k);
    E* e = (E*)malloc(sizeof(E));
    strncpy(e->k, k, 7); e->v = v; e->n = buckets[i]; buckets[i] = e;
}
int main(void) { put("x", 42); printf("%d\\n", buckets[h("x")]->v); return 0; }
"""),
    ("atomic_counter", ["Atomic", "Concurrency"], """
#include <stdio.h>
#include <stdatomic.h>
static atomic_int counter;
int main(void) {
    atomic_store(&counter, 0);
    for (int i = 0; i < 100; ++i) atomic_fetch_add(&counter, 1);
    printf("%d\\n", atomic_load(&counter));
    return 0;
}
"""),
    ("pthread_mutex", ["Mutex", "Concurrency", "Pthread"], """
#include <stdio.h>
#ifdef __linux__
#include <pthread.h>
static pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
static int g;
static void* worker(void* _) {
    (void)_;
    pthread_mutex_lock(&m);
    ++g;
    pthread_mutex_unlock(&m);
    return 0;
}
int main(void) {
    pthread_t t;
    pthread_create(&t, 0, worker, 0);
    pthread_join(t, 0);
    printf("%d\\n", g);
    return 0;
}
#else
int main(void) { printf("0\\n"); return 0; }
#endif
"""),
    ("bloom_filter", ["BloomFilter", "Probabilistic"], """
#include <stdio.h>
#include <stdint.h>
#define M 64
static uint64_t bits;
static void bf_add(const char* s) {
    uint32_t h = 0;
    while (*s) h = h*131 + (unsigned char)*s++;
    bits |= (1ULL << (h % M));
}
static int bf_maybe(const char* s) {
    uint32_t h = 0;
    while (*s) h = h*131 + (unsigned char)*s++;
    return (bits >> (h % M)) & 1;
}
int main(void) {
    bf_add("key");
    printf("%d\\n", bf_maybe("key"));
    return 0;
}
"""),
    ("lower_bound", ["LowerBound", "BinarySearch", "Search"], """
#include <stdio.h>
static int lower_bound(const int* a, int n, int x) {
    int lo = 0, hi = n;
    while (lo < hi) {
        int mid = lo + (hi - lo) / 2;
        if (a[mid] < x) lo = mid + 1; else hi = mid;
    }
    return lo;
}
int main(void) {
    int a[] = {1,2,2,3,4};
    printf("%d\\n", lower_bound(a, 5, 2));
    return 0;
}
"""),
    ("atoi_parse", ["Atoi", "Parse"], """
#include <stdio.h>
static int my_atoi(const char* s) {
    int n = 0, sign = 1;
    if (*s == '-') { sign = -1; ++s; }
    while (*s >= '0' && *s <= '9') { n = n*10 + (*s - '0'); ++s; }
    return sign * n;
}
int main(void) { printf("%d\\n", my_atoi("-1234")); return 0; }
"""),
    ("memset_loop", ["Memset", "Memory"], """
#include <stdio.h>
static void my_memset(unsigned char* p, unsigned char v, int n) {
    for (int i = 0; i < n; ++i) p[i] = v;
}
int main(void) {
    unsigned char b[4];
    my_memset(b, 0xAB, 4);
    printf("%02x\\n", b[0]);
    return 0;
}
"""),
]


def main() -> int:
    OUT.mkdir(parents=True, exist_ok=True)
    index = []
    for stem, algorithms, c_src in SPECS:
        c_path = OUT / f"{stem}.c"
        label_path = OUT / f"{stem}.labels.json"
        c_path.write_text(c_src.strip() + "\n", encoding="utf-8")
        label_path.write_text(
            json.dumps({"algorithms": algorithms, "functions": {}}, indent=2) + "\n",
            encoding="utf-8",
        )
        index.append({"stem": stem, "algorithms": algorithms})
    (OUT / "index.json").write_text(json.dumps(index, indent=2) + "\n", encoding="utf-8")
    print(f"Generated {len(SPECS)} sources in {OUT}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
