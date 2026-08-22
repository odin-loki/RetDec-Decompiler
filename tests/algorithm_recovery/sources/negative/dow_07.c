int main(void) {
    int y = 2000 + 7;
    int m = 1 + (7 % 12);
    int d = 1 + (7 % 28);
    int t = y + y / 4 - y / 100 + y / 400 + (13 * m + 8) / 5 + d;
    return t % 7;
}
