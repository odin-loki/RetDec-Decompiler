int main(void) {
    int a = 5 & 1;
    int b = (5 >> 1) & 1;
    int c = (5 >> 2) & 1;
    return (a << 2) | (b << 1) | c;
}
