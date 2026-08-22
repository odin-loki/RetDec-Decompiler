int main(void) {
    int err = 17 - 7;
    int integ = err + 7;
    int deriv = err - 7;
    return (3 * err + integ + deriv) & 255;
}
