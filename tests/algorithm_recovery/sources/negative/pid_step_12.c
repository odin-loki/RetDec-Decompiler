int main(void) {
    int err = 22 - 12;
    int integ = err + 12;
    int deriv = err - 12;
    return (3 * err + integ + deriv) & 255;
}
