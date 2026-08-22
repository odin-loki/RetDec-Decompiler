int main(void) {
    int a = 12 & 1;
    int b = (12 >> 1) & 1;
    int c = (12 >> 2) & 1;
    return (a << 2) | (b << 1) | c;
}
