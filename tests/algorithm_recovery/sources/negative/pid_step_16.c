int main(void) {
    int err = 26 - 16;
    int integ = err + 16;
    int deriv = err - 16;
    return (3 * err + integ + deriv) & 255;
}
