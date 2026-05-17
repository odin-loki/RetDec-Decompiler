#include <stdio.h>

const char *day_name(int d) {
    switch (d) {
        case 0: return "Sunday";
        case 1: return "Monday";
        case 2: return "Tuesday";
        case 3: return "Wednesday";
        case 4: return "Thursday";
        case 5: return "Friday";
        case 6: return "Saturday";
        default: return "Unknown";
    }
}

int classify(int x) {
    if (x < 0) return -1;
    if (x == 0) return 0;
    if (x < 10) return 1;
    if (x < 100) return 2;
    return 3;
}

int main(void) {
    for (int i = 0; i < 7; i++)
        printf("%d: %s\n", i, day_name(i));
    for (int i = -5; i <= 200; i += 50)
        printf("classify(%d) = %d\n", i, classify(i));
    return 0;
}
