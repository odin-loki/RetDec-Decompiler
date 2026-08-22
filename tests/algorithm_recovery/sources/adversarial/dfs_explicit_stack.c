/* DFS with an explicit stack. No recursive self-call (the assigned DFS heuristic). */
#include <stdio.h>

#define V 6

static int adj[V][V] = {
	{0, 1, 0, 1, 0, 0},
	{1, 0, 1, 0, 0, 0},
	{0, 1, 0, 0, 1, 0},
	{1, 0, 0, 0, 0, 1},
	{0, 0, 1, 0, 0, 1},
	{0, 0, 0, 1, 1, 0},
};
static int vis[V];
static int st[32];
static int top;

static void dfs_stack(int s)
{
	st[top++] = s;
	while (top > 0)
	{
		int u = st[--top];
		if (vis[u]) continue;
		vis[u] = 1;
		for (int v = V - 1; v >= 0; --v)
		{
			if (adj[u][v] && !vis[v]) st[top++] = v;
		}
	}
}

int main(void)
{
	dfs_stack(0);
	printf("%d\n", vis[4]);
	return 0;
}
