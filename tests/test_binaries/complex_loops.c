/* Complex loop patterns: exercises while_true_to_for_loop_optimizer */
#include <stdio.h>
#include <stdlib.h>

/* Nested loops with multiple exit points */
int find_2d(int m[4][4], int val) {
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            if (m[i][j] == val) return i * 4 + j;
        }
    }
    return -1;
}

/* Do-while loop */
int collatz(int n) {
    int steps = 0;
    do {
        if (n % 2 == 0) n /= 2;
        else n = 3 * n + 1;
        steps++;
    } while (n != 1);
    return steps;
}

/* Infinite loop with break */
int next_prime(int n) {
    while (1) {
        n++;
        int prime = 1;
        for (int i = 2; i * i <= n; i++) {
            if (n % i == 0) { prime = 0; break; }
        }
        if (prime) return n;
    }
}

/* Loop with continue */
void print_fizzbuzz(int limit) {
    for (int i = 1; i <= limit; i++) {
        if (i % 15 == 0) { puts("FizzBuzz"); continue; }
        if (i % 3 == 0)  { puts("Fizz"); continue; }
        if (i % 5 == 0)  { puts("Buzz"); continue; }
        printf("%d\n", i);
    }
}

/* While loop that becomes for */
int sum_while(int n) {
    int s = 0, i = 0;
    while (i < n) { s += i; i++; }
    return s;
}

int main(void) {
    int m[4][4] = {{1,2,3,4},{5,6,7,8},{9,10,11,12},{13,14,15,16}};
    printf("find(7)=%d\n", find_2d(m, 7));
    printf("collatz(27)=%d\n", collatz(27));
    printf("next_prime(10)=%d\n", next_prime(10));
    print_fizzbuzz(20);
    printf("sum_while(10)=%d\n", sum_while(10));
    return 0;
}
