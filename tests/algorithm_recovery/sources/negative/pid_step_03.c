int main(void) {
    int err = 13 - 3;
    int integ = err + 3;
    int deriv = err - 3;
    return (3 * err + integ + deriv) & 255;
}
