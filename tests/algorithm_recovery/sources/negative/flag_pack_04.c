int main(void) {
    int a = 4 & 1;
    int b = (4 >> 1) & 1;
    int c = (4 >> 2) & 1;
    return (a << 2) | (b << 1) | c;
}
