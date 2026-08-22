int main(void) {
    int principal = 100000 + 16 * 1000;
    int rate = 3 + (16 % 5);
    int years = 15 + (16 % 15);
    return (principal / 100) * rate / years;
}
