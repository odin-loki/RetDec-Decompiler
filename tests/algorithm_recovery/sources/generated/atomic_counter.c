#include <stdio.h>
#include <stdatomic.h>
static atomic_int counter;
int main(void) {
    atomic_store(&counter, 0);
    for (int i = 0; i < 100; ++i) atomic_fetch_add(&counter, 1);
    printf("%d\n", atomic_load(&counter));
    return 0;
}
