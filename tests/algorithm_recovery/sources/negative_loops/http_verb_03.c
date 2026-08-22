int main(void) {
    const char* p = "PUT /x";
    int st = 0;
    for (; *p; ++p) {
        char ch = *p;
        if (st == 0 && ch == 'G') st = 1;
        else if (st == 1 && ch == 'E') st = 2;
        else if (st == 2 && ch == 'T') st = 3;
        else if (st == 0 && ch == 'P') st = 4;
        else if (ch == ' ') break;
    }
    return st + 3;
}
