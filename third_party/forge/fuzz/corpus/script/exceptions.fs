try { throw std::runtime_error("bad thing"); }
catch (const std::exception& e) { std::println("caught: {}", e.what()); }
int z = 0;
try { std::println("{}", 1 / z); } catch (const std::exception& e) { std::println("div"); }
throw std::runtime_error("uncaught");
