int main(void) {
    int principal = 100000 + 11 * 1000;
    int rate = 3 + (11 % 5);
    int years = 15 + (11 % 15);
    return (principal / 100) * rate / years;
}
