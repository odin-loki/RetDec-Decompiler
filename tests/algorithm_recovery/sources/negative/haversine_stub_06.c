int main(void) {
    int lat = 6 * 3;
    int lon = 6 * 5;
    int d = lat * lat + lon * lon;
    return d & 1023;
}
