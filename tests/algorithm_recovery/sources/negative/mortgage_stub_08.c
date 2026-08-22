int main(void) {
    int principal = 100000 + 8 * 1000;
    int rate = 3 + (8 % 5);
    int years = 15 + (8 % 15);
    return (principal / 100) * rate / years;
}
