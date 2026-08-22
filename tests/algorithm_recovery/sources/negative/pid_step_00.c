int main(void) {
    int err = 10 - 0;
    int integ = err + 0;
    int deriv = err - 0;
    return (3 * err + integ + deriv) & 255;
}
