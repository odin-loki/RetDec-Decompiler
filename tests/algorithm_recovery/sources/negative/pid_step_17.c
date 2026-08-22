int main(void) {
    int err = 27 - 17;
    int integ = err + 17;
    int deriv = err - 17;
    return (3 * err + integ + deriv) & 255;
}
