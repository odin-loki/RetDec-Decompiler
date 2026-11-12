## Eigen

A clone of only the needed sources (headers) of [Eigen](http://eigen.tuxfamily.org/index.php?title=Main_Page).
Upstream commit: `6601abce868e3284b4829a4fbf91eefaa0d704af`

## License

Eigen is [Free Software](http://www.gnu.org/philosophy/free-sw.html). Starting from the 3.1.1 version, it is licensed under the [MPL2](http://www.mozilla.org/MPL/2.0), which is a simple weak copyleft license. Common questions about the MPL2 are answered in the official [MPL2 FAQ](http://www.mozilla.org/MPL/2.0/FAQ.html).

RetDec patch: `Eigen/src/Core/util/Meta.h` uses `std::invoke_result` instead of `std::result_of` under C++17 and later (backport of Eigen 3.3.8), because C++20 libc++ no longer has `std::result_of`.
