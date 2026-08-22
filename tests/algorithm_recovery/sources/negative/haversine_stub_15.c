int main(void) {
    int lat = 15 * 3;
    int lon = 15 * 5;
    int d = lat * lat + lon * lon;
    return d & 1023;
}
