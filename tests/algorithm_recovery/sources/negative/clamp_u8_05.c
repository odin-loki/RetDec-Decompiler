int main(void) {
    int x = 255;
    if (x < 0) x = 0;
    if (x > 255) x = 255;
    return x;
}
