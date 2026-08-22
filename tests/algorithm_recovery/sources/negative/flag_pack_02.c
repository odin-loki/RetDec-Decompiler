int main(void) {
    int a = 2 & 1;
    int b = (2 >> 1) & 1;
    int c = (2 >> 2) & 1;
    return (a << 2) | (b << 1) | c;
}
