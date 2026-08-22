int main(void) {
    int principal = 100000 + 14 * 1000;
    int rate = 3 + (14 % 5);
    int years = 15 + (14 % 15);
    return (principal / 100) * rate / years;
}
