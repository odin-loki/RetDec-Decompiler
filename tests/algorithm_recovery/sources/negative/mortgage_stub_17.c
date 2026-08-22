int main(void) {
    int principal = 100000 + 17 * 1000;
    int rate = 3 + (17 % 5);
    int years = 15 + (17 % 15);
    return (principal / 100) * rate / years;
}
