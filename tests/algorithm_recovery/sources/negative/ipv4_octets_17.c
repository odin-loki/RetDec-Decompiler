int main(void) {
    unsigned v = 0x0A000000u + 17u;
    return (int)((v >> 24) & 255u);
}
