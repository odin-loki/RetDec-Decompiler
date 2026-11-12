// FUSE UI documents: a small strict JSON reader (RFC 8259; // and /* */ comments and trailing commas accepted, as
// authoring conveniences for .fuseui / .fusestyle side files). Errors carry line and column; no exceptions.
#pragma once

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fuse::ui::doc {

struct JsonValue {
    enum class Type : std::uint8_t { Null, Bool, Number, String, Array, Object };
    Type type = Type::Null;
    bool boolean = false;
    double number = 0.0;
    std::string string;
    std::vector<JsonValue> array;
    std::vector<std::pair<std::string, JsonValue>> object; ///< insertion order kept

    bool isNull() const { return type == Type::Null; }
    bool isBool() const { return type == Type::Bool; }
    bool isNumber() const { return type == Type::Number; }
    bool isString() const { return type == Type::String; }
    bool isArray() const { return type == Type::Array; }
    bool isObject() const { return type == Type::Object; }
    /// Object member or null.
    const JsonValue* get(std::string_view key) const;
};

struct JsonError {
    std::uint32_t line = 1, column = 1;
    std::string message;
};

std::expected<JsonValue, JsonError> parseJson(std::string_view text);

} // namespace fuse::ui::doc
