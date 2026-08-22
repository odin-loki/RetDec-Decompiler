int main(void) {
    int a = 8 & 1;
    int b = (8 >> 1) & 1;
    int c = (8 >> 2) & 1;
    return (a << 2) | (b << 1) | c;
}
