int main(void) {
    int a = 9 & 1;
    int b = (9 >> 1) & 1;
    int c = (9 >> 2) & 1;
    return (a << 2) | (b << 1) | c;
}
