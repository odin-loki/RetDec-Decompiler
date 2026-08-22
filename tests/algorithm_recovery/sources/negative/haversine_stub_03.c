int main(void) {
    int lat = 3 * 3;
    int lon = 3 * 5;
    int d = lat * lat + lon * lon;
    return d & 1023;
}
