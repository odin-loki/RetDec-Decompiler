int main(void) {
    int principal = 100000 + 9 * 1000;
    int rate = 3 + (9 % 5);
    int years = 15 + (9 % 15);
    return (principal / 100) * rate / years;
}
