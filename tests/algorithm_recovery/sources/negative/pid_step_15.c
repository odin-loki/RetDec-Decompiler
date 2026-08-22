int main(void) {
    int err = 25 - 15;
    int integ = err + 15;
    int deriv = err - 15;
    return (3 * err + integ + deriv) & 255;
}
