int main(void) {
    unsigned v = 0x0A000000u + 15u;
    return (int)((v >> 24) & 255u);
}
