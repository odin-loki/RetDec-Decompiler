int main(void) {
    int lat = 8 * 3;
    int lon = 8 * 5;
    int d = lat * lat + lon * lon;
    return d & 1023;
}
