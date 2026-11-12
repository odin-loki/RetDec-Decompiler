int total = 0;
for (int i = 0; i < 10; ++i) { if (i % 2) continue; if (i > 6) break; total += i; }
std::println("{}", total);
int n = 0; while (n < 5) ++n;
std::println("{}", n);
int k = 3;
switch (k) { case 1: std::println("one"); break; case 3: std::println("three"); break; default: std::println("other"); }
