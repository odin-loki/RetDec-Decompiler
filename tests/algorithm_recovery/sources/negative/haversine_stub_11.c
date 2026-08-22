int main(void) {
    int lat = 11 * 3;
    int lon = 11 * 5;
    int d = lat * lat + lon * lon;
    return d & 1023;
}
