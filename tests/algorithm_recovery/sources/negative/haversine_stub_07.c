int main(void) {
    int lat = 7 * 3;
    int lon = 7 * 5;
    int d = lat * lat + lon * lon;
    return d & 1023;
}
