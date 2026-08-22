int main(void) {
    int principal = 100000 + 4 * 1000;
    int rate = 3 + (4 % 5);
    int years = 15 + (4 % 15);
    return (principal / 100) * rate / years;
}
