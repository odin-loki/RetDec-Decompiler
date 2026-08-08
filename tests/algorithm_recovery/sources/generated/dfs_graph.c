#include <stdio.h>
#define V 4
static int adj[V][V] = {{0,1,1,0},{1,0,0,1},{1,0,0,1},{0,1,1,0}};
static int vis[V];
static void dfs(int u) {
    vis[u] = 1;
    for (int v = 0; v < V; ++v)
        if (adj[u][v] && !vis[v]) dfs(v);
}
int main(void) { dfs(0); printf("%d\n", vis[3]); return 0; }
