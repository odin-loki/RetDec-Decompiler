int main(void) {
    int principal = 100000 + 6 * 1000;
    int rate = 3 + (6 % 5);
    int years = 15 + (6 % 15);
    return (principal / 100) * rate / years;
}
