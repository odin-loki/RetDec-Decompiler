std::string s = "forge";
std::println("{} {} {}", s.size(), s + "!", s.substr(1, 3));
std::string up;
for (char c : s) up += std::toupper(c);
std::println("{}", up);
std::println("{}", s.find("rg"));
