int main(void) {
    int principal = 100000 + 10 * 1000;
    int rate = 3 + (10 % 5);
    int years = 15 + (10 % 15);
    return (principal / 100) * rate / years;
}
