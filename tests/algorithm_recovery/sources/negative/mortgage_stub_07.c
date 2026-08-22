int main(void) {
    int principal = 100000 + 7 * 1000;
    int rate = 3 + (7 % 5);
    int years = 15 + (7 % 15);
    return (principal / 100) * rate / years;
}
