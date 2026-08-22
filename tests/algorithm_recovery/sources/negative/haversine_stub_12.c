int main(void) {
    int lat = 12 * 3;
    int lon = 12 * 5;
    int d = lat * lat + lon * lon;
    return d & 1023;
}
