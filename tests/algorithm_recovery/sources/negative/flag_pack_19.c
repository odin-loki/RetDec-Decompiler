int main(void) {
    int a = 19 & 1;
    int b = (19 >> 1) & 1;
    int c = (19 >> 2) & 1;
    return (a << 2) | (b << 1) | c;
}
