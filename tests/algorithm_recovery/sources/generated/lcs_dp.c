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
int main(void) { printf("%d\n", lcs("abcde", "ace")); return 0; }
