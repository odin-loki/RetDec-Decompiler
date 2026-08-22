int main(void) {
    int err = 23 - 13;
    int integ = err + 13;
    int deriv = err - 13;
    return (3 * err + integ + deriv) & 255;
}
