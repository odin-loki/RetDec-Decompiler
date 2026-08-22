int main(void) {
    int a = 11 & 1;
    int b = (11 >> 1) & 1;
    int c = (11 >> 2) & 1;
    return (a << 2) | (b << 1) | c;
}
