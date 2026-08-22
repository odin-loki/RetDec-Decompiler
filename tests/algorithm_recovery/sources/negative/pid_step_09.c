int main(void) {
    int err = 19 - 9;
    int integ = err + 9;
    int deriv = err - 9;
    return (3 * err + integ + deriv) & 255;
}
