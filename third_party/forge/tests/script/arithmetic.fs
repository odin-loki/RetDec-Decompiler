std::println("{} {} {}", 1 + 2 * 3, 17 / 5, 17 % 5);
std::println("{} {}", -7 / 2, 7.0 / 2);
std::println("{}", (1 << 10) | 3);
long long big = 9223372036854775807;
try { big + 1; } catch (const std::exception& e) { std::println("overflow caught"); }
