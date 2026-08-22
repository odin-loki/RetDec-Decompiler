int main(void) {
    int principal = 100000 + 12 * 1000;
    int rate = 3 + (12 % 5);
    int years = 15 + (12 % 15);
    return (principal / 100) * rate / years;
}
