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
    printf("%d\n", knapsack(w,v,3,7));
    return 0;
}
