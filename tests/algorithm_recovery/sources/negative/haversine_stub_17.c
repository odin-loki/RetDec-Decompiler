int main(void) {
    int lat = 17 * 3;
    int lon = 17 * 5;
    int d = lat * lat + lon * lon;
    return d & 1023;
}
