int main(void) {
    int err = 16 - 6;
    int integ = err + 6;
    int deriv = err - 6;
    return (3 * err + integ + deriv) & 255;
}
