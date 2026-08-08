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
int main(void) { bfs(0); printf("%d\n", vis[2]); return 0; }
