int main(void) {
    int x0 = 0, y0 = 0, x1 = 8, y1 = 5;
    int dx = x1 - x0, dy = y1 - y0, err = dx - dy, plot = 0;
    int x = x0, y = y0;
    for (int s = 0; s < 22; ++s) {
        plot += x + y;
        int e2 = err * 2;
        if (e2 > -dy) { err -= dy; x += 1; }
        if (e2 < dx) { err += dx; y += 1; }
        if (x >= x1 && y >= y1) break;
    }
    return plot & 255;
}
