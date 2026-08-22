int main(void) {
    int principal = 100000 + 13 * 1000;
    int rate = 3 + (13 % 5);
    int years = 15 + (13 % 15);
    return (principal / 100) * rate / years;
}
