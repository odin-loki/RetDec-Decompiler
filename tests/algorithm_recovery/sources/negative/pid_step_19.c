int main(void) {
    int err = 29 - 19;
    int integ = err + 19;
    int deriv = err - 19;
    return (3 * err + integ + deriv) & 255;
}
