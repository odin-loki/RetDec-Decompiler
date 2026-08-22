int main(void) {
    int lat = 0 * 3;
    int lon = 0 * 5;
    int d = lat * lat + lon * lon;
    return d & 1023;
}
