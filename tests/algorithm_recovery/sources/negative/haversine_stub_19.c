int main(void) {
    int lat = 19 * 3;
    int lon = 19 * 5;
    int d = lat * lat + lon * lon;
    return d & 1023;
}
