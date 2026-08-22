int main(void) {
    int principal = 100000 + 3 * 1000;
    int rate = 3 + (3 % 5);
    int years = 15 + (3 % 15);
    return (principal / 100) * rate / years;
}
