int main(void) {
    int h[16] = {0};
    int v = 22;
    for (int k = 0; k < 32; ++k) {
        int bin = (v + k) & 15;
        h[bin] += 1;
        v = v * 17 + 1;
    }
    return h[2] & 255;
}
