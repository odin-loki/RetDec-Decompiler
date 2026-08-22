int main(void) {
    int lat = 18 * 3;
    int lon = 18 * 5;
    int d = lat * lat + lon * lon;
    return d & 1023;
}
