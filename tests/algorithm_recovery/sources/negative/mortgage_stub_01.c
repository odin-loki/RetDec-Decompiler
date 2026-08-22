int main(void) {
    int principal = 100000 + 1 * 1000;
    int rate = 3 + (1 % 5);
    int years = 15 + (1 % 15);
    return (principal / 100) * rate / years;
}
