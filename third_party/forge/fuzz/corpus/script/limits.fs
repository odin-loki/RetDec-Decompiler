int r = forge::limit(std::map<std::string, double>{{"timeout", 5000}, {"cpu_s", 5.0}}, [] { return 7 * 6; });
std::println("{}", r);
try { forge::limit(std::map<std::string, int>{{"timeout", 20}}, [] { while (true) {} }); }
catch (const std::exception& e) { std::println("timed out"); }
