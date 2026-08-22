int main(void) {
    int err = 20 - 10;
    int integ = err + 10;
    int deriv = err - 10;
    return (3 * err + integ + deriv) & 255;
}
