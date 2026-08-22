int main(void) {
    int a = 15 & 1;
    int b = (15 >> 1) & 1;
    int c = (15 >> 2) & 1;
    return (a << 2) | (b << 1) | c;
}
