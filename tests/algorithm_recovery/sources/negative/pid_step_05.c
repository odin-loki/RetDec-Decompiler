int main(void) {
    int err = 15 - 5;
    int integ = err + 5;
    int deriv = err - 5;
    return (3 * err + integ + deriv) & 255;
}
