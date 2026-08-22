int main(void) {
    int a = 3 & 1;
    int b = (3 >> 1) & 1;
    int c = (3 >> 2) & 1;
    return (a << 2) | (b << 1) | c;
}
