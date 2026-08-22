int main(void) {
    int lat = 13 * 3;
    int lon = 13 * 5;
    int d = lat * lat + lon * lon;
    return d & 1023;
}
