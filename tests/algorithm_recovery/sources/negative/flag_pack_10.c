int main(void) {
    int a = 10 & 1;
    int b = (10 >> 1) & 1;
    int c = (10 >> 2) & 1;
    return (a << 2) | (b << 1) | c;
}
