int main(void) {
    int lat = 5 * 3;
    int lon = 5 * 5;
    int d = lat * lat + lon * lon;
    return d & 1023;
}
