int main(void) {
    int principal = 100000 + 2 * 1000;
    int rate = 3 + (2 % 5);
    int years = 15 + (2 % 15);
    return (principal / 100) * rate / years;
}
