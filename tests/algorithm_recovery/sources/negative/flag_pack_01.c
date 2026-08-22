int main(void) {
    int a = 1 & 1;
    int b = (1 >> 1) & 1;
    int c = (1 >> 2) & 1;
    return (a << 2) | (b << 1) | c;
}
