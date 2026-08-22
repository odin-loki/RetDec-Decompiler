int main(void) {
    int lat = 2 * 3;
    int lon = 2 * 5;
    int d = lat * lat + lon * lon;
    return d & 1023;
}
