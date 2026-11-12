std::vector<int> v = {5, 3, 9, 1};
std::sort(v.begin(), v.end());
for (int x : v) std::print("{} ", x);
std::println("");
std::map<std::string, int> m;
m["b"] = 2; m["a"] = 1;
for (auto& [k, x] : m) std::println("{}={}", k, x);
std::println("{}", v.size() + m.size());
