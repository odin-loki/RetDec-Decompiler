int main() {
    std::println("{}", forge::text::upper("straße"));
    std::println("{}", forge::locale::number(1234567.891, "de"));
    std::println("{}", forge::locale::currency(-5, "USD", "en"));
    std::println("{}", forge::locale::list({"a","b","c"}, "es"));
    std::println("{}", forge::locale::message("{n, plural, one {# item} other {# items}}", {{"n", 3}}, "en"));
    return 0;
}
