int main(void) {
    int err = 28 - 18;
    int integ = err + 18;
    int deriv = err - 18;
    return (3 * err + integ + deriv) & 255;
}
