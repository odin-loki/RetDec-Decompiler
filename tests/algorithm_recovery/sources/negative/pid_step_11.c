int main(void) {
    int err = 21 - 11;
    int integ = err + 11;
    int deriv = err - 11;
    return (3 * err + integ + deriv) & 255;
}
