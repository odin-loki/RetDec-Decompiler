int main(void) {
    int lat = 9 * 3;
    int lon = 9 * 5;
    int d = lat * lat + lon * lon;
    return d & 1023;
}
