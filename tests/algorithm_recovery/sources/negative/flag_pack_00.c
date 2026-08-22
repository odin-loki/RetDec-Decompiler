int main(void) {
    int a = 0 & 1;
    int b = (0 >> 1) & 1;
    int c = (0 >> 2) & 1;
    return (a << 2) | (b << 1) | c;
}
