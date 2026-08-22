int main(void) {
    int a = 13 & 1;
    int b = (13 >> 1) & 1;
    int c = (13 >> 2) & 1;
    return (a << 2) | (b << 1) | c;
}
