int main(void) {
    int y = 2000 + 18;
    int m = 1 + (18 % 12);
    int d = 1 + (18 % 28);
    int t = y + y / 4 - y / 100 + y / 400 + (13 * m + 8) / 5 + d;
    return t % 7;
}
