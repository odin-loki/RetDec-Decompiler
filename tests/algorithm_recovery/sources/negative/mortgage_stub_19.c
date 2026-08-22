int main(void) {
    int principal = 100000 + 19 * 1000;
    int rate = 3 + (19 % 5);
    int years = 15 + (19 % 15);
    return (principal / 100) * rate / years;
}
