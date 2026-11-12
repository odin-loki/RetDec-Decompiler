// Runtime reflection in FORGE script: run with `forge script docs/examples/script/reflection.cpp`
struct [[serializable]] Point {
    [[key("x")]] double x = 0;
    double y = 0;
    const int id = 7;
    Point(double a, double b) : x(a), y(b) {}
    double norm2() const { return x * x + y * y; }
    void scale(double k) { x *= k; y *= k; }
};
enum class Color { Red, Green, Blue };
int add(int a, int b = 2) { return a + b; }

int main() {
    auto p = forge::reflect::construct("Point", 3.0, 4.0);
    std::println("{} is a {}", forge::typeof(p), forge::reflect::kind(p));
    for (const auto& f : forge::reflect::fields(Point)) std::println("field {} : {}", f["name"], f["type"]);
    for (const auto& m : forge::reflect::methods(Point)) std::println("method {}", m["name"]);
    std::println("attrs {}", forge::reflect::attributes(Point).size());
    std::println("norm2 {}", forge::reflect::invoke(p, "norm2"));
    forge::reflect::set(p, "x", 6.0);
    std::println("x {}", forge::reflect::get(p, "x"));
    try { forge::reflect::set(p, "id", 1); } catch (const std::exception& e) { std::println("const refused"); }
    for (const auto& e : forge::reflect::enum_values(Color)) std::println("{} = {}", e["name"], e["value"]);
    auto s = forge::reflect::signature(add);
    std::println("{} returns {} arity {}", s["name"], s["returns"], s["arity"]);
    std::println("call {}", forge::reflect::call("add", 40));
    return 0;
}
