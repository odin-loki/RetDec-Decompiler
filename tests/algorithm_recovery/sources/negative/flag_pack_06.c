int main(void) {
    int a = 6 & 1;
    int b = (6 >> 1) & 1;
    int c = (6 >> 2) & 1;
    return (a << 2) | (b << 1) | c;
}
