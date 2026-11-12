int fib(int n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }
std::println("{}", fib(20));
auto add = [](int a, int b) { return a + b; };
std::println("{}", add(2, 40));
int counter = 0;
auto bump = [&] { ++counter; };
bump(); bump(); bump();
std::println("{}", counter);
