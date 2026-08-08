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
    printf("%d\n", g);
    return 0;
}
#else
int main(void) { printf("0\n"); return 0; }
#endif
