int main(void) {
    int a = 16 & 1;
    int b = (16 >> 1) & 1;
    int c = (16 >> 2) & 1;
    return (a << 2) | (b << 1) | c;
}
