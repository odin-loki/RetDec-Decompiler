int main(void) {
    int lat = 10 * 3;
    int lon = 10 * 5;
    int d = lat * lat + lon * lon;
    return d & 1023;
}
