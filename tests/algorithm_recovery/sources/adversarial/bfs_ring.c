/* BFS over an adjacency matrix with an explicit circular queue (head/tail % CAP). */
#include <stdio.h>

#define V 6
#define CAP 16

static int adj[V][V] = {
	{0, 1, 0, 1, 0, 0},
	{1, 0, 1, 0, 1, 0},
	{0, 1, 0, 1, 0, 1},
	{1, 0, 1, 0, 0, 0},
	{0, 1, 0, 0, 0, 1},
	{0, 0, 1, 0, 1, 0},
};
static int vis[V];
static int q[CAP];
static int head;
static int tail;
static int used;

static void enq(int x)
{
	q[tail] = x;
	tail = (tail + 1) % CAP;
	++used;
}

static int deq(void)
{
	int x = q[head];
	head = (head + 1) % CAP;
	--used;
	return x;
}

static void bfs_ring(int s)
{
	vis[s] = 1;
	enq(s);
	while (used > 0)
	{
		int u = deq();
		for (int v = 0; v < V; ++v)
		{
			if (adj[u][v] && !vis[v])
			{
				vis[v] = 1;
				enq(v);
			}
		}
	}
}

int main(void)
{
	bfs_ring(0);
	printf("%d\n", vis[5]);
	return 0;
}
