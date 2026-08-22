int main(void) {
    int err = 18 - 8;
    int integ = err + 8;
    int deriv = err - 8;
    return (3 * err + integ + deriv) & 255;
}
