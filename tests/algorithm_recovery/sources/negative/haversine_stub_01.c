int main(void) {
    int lat = 1 * 3;
    int lon = 1 * 5;
    int d = lat * lat + lon * lon;
    return d & 1023;
}
