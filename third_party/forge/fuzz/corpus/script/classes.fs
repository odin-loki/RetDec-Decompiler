struct Point { int x = 0; int y = 0; int norm1() const { return std::abs(x) + std::abs(y); } };
Point p; p.x = 3; p.y = -4;
std::println("{}", p.norm1());
class Shape { public: virtual int area() const { return 0; } };
class Sq : public Shape { public: int s = 0; int area() const override { return s * s; } };
Sq q; q.s = 5;
std::println("{}", q.area());
