int main(void) {
    int lat = 14 * 3;
    int lon = 14 * 5;
    int d = lat * lat + lon * lon;
    return d & 1023;
}
