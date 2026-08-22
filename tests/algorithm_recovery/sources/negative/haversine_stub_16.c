int main(void) {
    int lat = 16 * 3;
    int lon = 16 * 5;
    int d = lat * lat + lon * lon;
    return d & 1023;
}
