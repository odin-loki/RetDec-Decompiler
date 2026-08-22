int main(void) {
    int a = 18 & 1;
    int b = (18 >> 1) & 1;
    int c = (18 >> 2) & 1;
    return (a << 2) | (b << 1) | c;
}
