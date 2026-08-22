int main(void) {
    int m[4][4];
    int t[4][4];
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) m[r][c] = r * 4 + c + 0;
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) t[c][r] = m[r][c];
    return t[1][2] & 255;
}
