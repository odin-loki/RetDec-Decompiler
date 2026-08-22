int main(void) {
    int err = 14 - 4;
    int integ = err + 4;
    int deriv = err - 4;
    return (3 * err + integ + deriv) & 255;
}
