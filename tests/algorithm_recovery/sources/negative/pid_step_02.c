int main(void) {
    int err = 12 - 2;
    int integ = err + 2;
    int deriv = err - 2;
    return (3 * err + integ + deriv) & 255;
}
