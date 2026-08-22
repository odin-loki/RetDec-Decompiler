int main(void) {
    const unsigned char s[] = {0xC3, 0xA9, 0x61, 0x00};
    int n = 0;
    for (int k = 0; s[k]; ++k) {
        unsigned char b = s[k];
        if ((b & 0x80u) == 0) n += 1;
        else if ((b & 0xE0u) == 0xC0u) n += 2;
        else n += 3;
    }
    return (n + 3) & 255;
}
