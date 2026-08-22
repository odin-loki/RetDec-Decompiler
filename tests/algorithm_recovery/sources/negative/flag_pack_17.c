int main(void) {
    int a = 17 & 1;
    int b = (17 >> 1) & 1;
    int c = (17 >> 2) & 1;
    return (a << 2) | (b << 1) | c;
}
