int main(void) {
    int lat = 4 * 3;
    int lon = 4 * 5;
    int d = lat * lat + lon * lon;
    return d & 1023;
}
