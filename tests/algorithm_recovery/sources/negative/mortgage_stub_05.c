int main(void) {
    int principal = 100000 + 5 * 1000;
    int rate = 3 + (5 % 5);
    int years = 15 + (5 % 15);
    return (principal / 100) * rate / years;
}
