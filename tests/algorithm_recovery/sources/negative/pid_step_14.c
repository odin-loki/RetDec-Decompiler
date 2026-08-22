int main(void) {
    int err = 24 - 14;
    int integ = err + 14;
    int deriv = err - 14;
    return (3 * err + integ + deriv) & 255;
}
