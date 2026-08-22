int main(void) {
    int a = 7 & 1;
    int b = (7 >> 1) & 1;
    int c = (7 >> 2) & 1;
    return (a << 2) | (b << 1) | c;
}
