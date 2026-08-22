int main(void) {
    int err = 11 - 1;
    int integ = err + 1;
    int deriv = err - 1;
    return (3 * err + integ + deriv) & 255;
}
