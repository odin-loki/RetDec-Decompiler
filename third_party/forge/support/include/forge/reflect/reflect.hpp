#pragma once
// reflect: field-list reflection for serialisation (save games, config, messages). Macro form today; the
// same field tuple is what a C++26 reflection backend would generate.
//   struct cfg { int w; std::string name; std::vector<float> v; };  FORGE_REFLECT(cfg, w, name, v)
//   json::value j = reflect::to_json(c);   cfg c2 = reflect::from_json<cfg>(j);
#include <forge/json/json.hpp>
#include <map>
#include <string>
#include <tuple>
#include <type_traits>
#include <vector>

#define FORGE_RF_EXPAND(x) x
#define FORGE_RF_F(T, f) std::pair<const char*, decltype(&T::f)>{#f, &T::f}
#define FORGE_RF_1(T, a) FORGE_RF_F(T, a)
#define FORGE_RF_2(T, a, ...) FORGE_RF_F(T, a), FORGE_RF_EXPAND(FORGE_RF_1(T, __VA_ARGS__))
#define FORGE_RF_3(T, a, ...) FORGE_RF_F(T, a), FORGE_RF_EXPAND(FORGE_RF_2(T, __VA_ARGS__))
#define FORGE_RF_4(T, a, ...) FORGE_RF_F(T, a), FORGE_RF_EXPAND(FORGE_RF_3(T, __VA_ARGS__))
#define FORGE_RF_5(T, a, ...) FORGE_RF_F(T, a), FORGE_RF_EXPAND(FORGE_RF_4(T, __VA_ARGS__))
#define FORGE_RF_6(T, a, ...) FORGE_RF_F(T, a), FORGE_RF_EXPAND(FORGE_RF_5(T, __VA_ARGS__))
#define FORGE_RF_7(T, a, ...) FORGE_RF_F(T, a), FORGE_RF_EXPAND(FORGE_RF_6(T, __VA_ARGS__))
#define FORGE_RF_8(T, a, ...) FORGE_RF_F(T, a), FORGE_RF_EXPAND(FORGE_RF_7(T, __VA_ARGS__))
#define FORGE_RF_9(T, a, ...) FORGE_RF_F(T, a), FORGE_RF_EXPAND(FORGE_RF_8(T, __VA_ARGS__))
#define FORGE_RF_10(T, a, ...) FORGE_RF_F(T, a), FORGE_RF_EXPAND(FORGE_RF_9(T, __VA_ARGS__))
#define FORGE_RF_11(T, a, ...) FORGE_RF_F(T, a), FORGE_RF_EXPAND(FORGE_RF_10(T, __VA_ARGS__))
#define FORGE_RF_12(T, a, ...) FORGE_RF_F(T, a), FORGE_RF_EXPAND(FORGE_RF_11(T, __VA_ARGS__))
#define FORGE_RF_N(_1, _2, _3, _4, _5, _6, _7, _8, _9, _10, _11, _12, N, ...) N
#define FORGE_RF_CAT2(a, b) a##b
#define FORGE_RF_CAT(a, b) FORGE_RF_CAT2(a, b)
#define FORGE_REFLECT(T, ...)                                                                               \
    inline constexpr auto forge_reflect_fields(const T*) {                                                  \
        return std::make_tuple(FORGE_RF_EXPAND(FORGE_RF_CAT(FORGE_RF_, FORGE_RF_N(__VA_ARGS__, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1))(T, __VA_ARGS__))); \
    }

namespace forge::reflect {
template <class T> concept reflected = requires(const T* p) { forge_reflect_fields(p); };
template <class T> struct is_vec : std::false_type {};
template <class U, class A> struct is_vec<std::vector<U, A>> : std::true_type {};
template <class T> struct is_smap : std::false_type {};
template <class U> struct is_smap<std::map<std::string, U>> : std::true_type {};

template <class T> json::value to_json(const T& v) {
    if constexpr (std::is_same_v<T, bool>) return json::value(v);
    else if constexpr (std::is_enum_v<T>) return json::value(double(std::underlying_type_t<T>(v)));
    else if constexpr (std::is_arithmetic_v<T>) return json::value(double(v));
    else if constexpr (std::is_same_v<T, std::string>) return json::value(v);
    else if constexpr (is_vec<T>::value) { json::array a; for (auto& e : v) a.push_back(to_json(e)); return json::value(std::move(a)); }
    else if constexpr (is_smap<T>::value) { json::object o; for (auto& [k, e] : v) o.emplace_back(k, to_json(e)); return json::value(std::move(o)); }
    else {
        static_assert(reflected<T>, "reflect::to_json: type has no FORGE_REFLECT");
        json::object o;
        std::apply([&](auto... f) { (o.emplace_back(f.first, to_json(v.*(f.second))), ...); }, forge_reflect_fields(static_cast<const T*>(nullptr)));
        return json::value(std::move(o));
    }
}
template <class T> T from_json(const json::value& j) {
    if constexpr (std::is_same_v<T, bool>) return j.b();
    else if constexpr (std::is_enum_v<T>) return T(std::underlying_type_t<T>(j.num()));
    else if constexpr (std::is_arithmetic_v<T>) return T(j.num());
    else if constexpr (std::is_same_v<T, std::string>) return j.str();
    else if constexpr (is_vec<T>::value) { T out; for (auto& e : j.arr()) out.push_back(from_json<typename T::value_type>(e)); return out; }
    else if constexpr (is_smap<T>::value) { T out; for (auto& [k, e] : j.obj()) out[k] = from_json<typename T::mapped_type>(e); return out; }
    else {
        static_assert(reflected<T>, "reflect::from_json: type has no FORGE_REFLECT");
        T out{};
        std::apply([&](auto... f) {   // missing keys keep their defaults (forward-compatible save files)
            ((j.find(f.first) ? void(out.*(f.second) = from_json<std::remove_cvref_t<decltype(out.*(f.second))>>(*j.find(f.first))) : void()), ...);
        }, forge_reflect_fields(static_cast<const T*>(nullptr)));
        return out;
    }
}
} // namespace forge::reflect
