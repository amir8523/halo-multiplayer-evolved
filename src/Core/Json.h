// SPDX-License-Identifier: MIT
// ForgeEvolved: Core/Json.h
//
// Self contained JSON reader and writer.
//
// WHY NOT A LIBRARY
//
// Fetching a dependency at configure time means a contributor cannot build
// offline, and vendoring a large header costs more to review than this file. The
// subset JSON needs here is small and completely specified, so it is implemented
// directly. The project now has zero third party dependencies: nothing to fetch,
// nothing to license, nothing to keep in sync.
//
// API SHAPE
//
// The public surface deliberately mirrors the subset of nlohmann/json that the
// map parser and the symbol descriptor loader use (contains, at, is_*, get<T>,
// size, push_back, dump). That keeps those files idiomatic and makes swapping in
// a full library later a one line change if anyone ever wants to.
//
// GUARANTEES
//
//   Insertion ordered objects. Writing a parsed document reproduces its key
//   order, so a diff between two map revisions shows only real changes.
//
//   Bounded. Nesting depth and document length are capped, so a hostile document
//   cannot exhaust the stack through recursion.
//
//   Exact number classification. is_number_unsigned and is_number_integer report
//   what was actually written, so a field declared as a count cannot silently
//   accept -1 or 2.5.
#pragma once

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <istream>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fe::json {

/// Thrown for any malformed document. Contained at the call site in every
/// consumer; nothing in this project lets a JSON exception escape.
class exception : public std::runtime_error {
public:
    explicit exception(const std::string& message) : std::runtime_error(message) {}
};

/// Syntax error, carrying the byte offset so a message can point at the problem.
class parse_error : public exception {
public:
    parse_error(std::size_t byte_offset, const std::string& message)
        : exception(message), byte(byte_offset) {}

    std::size_t byte;
};

/// Type error, for a get<T> or at() that does not match the stored value.
class type_error : public exception {
public:
    explicit type_error(const std::string& message) : exception(message) {}
};

/// Maximum nesting depth. A legitimate map document nests four levels; 64 is far
/// beyond that and keeps the recursive parser inside the stack.
inline constexpr int kMaxDepth = 64;

class Value;

/// One JSON value.
class Value {
public:
    enum class Type : std::uint8_t {
        Null = 0,
        Object,
        Array,
        String,
        Number,
        Boolean,
    };

    /// How a number was written, so integer typed fields can reject 2.5 and -1.
    enum class NumberKind : std::uint8_t {
        Unsigned = 0,
        Signed,
        Real,
    };

    // --- Construction -----------------------------------------------------

    Value() = default;
    Value(std::nullptr_t) {}

    Value(bool value) : type_(Type::Boolean), boolean_(value) {}

    Value(std::uint8_t value)  : Value(static_cast<std::uint64_t>(value)) {}
    Value(std::uint16_t value) : Value(static_cast<std::uint64_t>(value)) {}
    Value(std::uint32_t value) : Value(static_cast<std::uint64_t>(value)) {}
    Value(std::uint64_t value)
        : type_(Type::Number), number_kind_(NumberKind::Unsigned), unsigned_(value) {}

    Value(std::int8_t value)  : Value(static_cast<std::int64_t>(value)) {}
    Value(std::int16_t value) : Value(static_cast<std::int64_t>(value)) {}
    Value(std::int32_t value) : Value(static_cast<std::int64_t>(value)) {}
    Value(std::int64_t value);

    Value(float value)  : Value(static_cast<double>(value)) {}
    Value(double value) : type_(Type::Number), number_kind_(NumberKind::Real), real_(value) {}

    Value(const char* value) : type_(Type::String), string_(value == nullptr ? "" : value) {}
    Value(std::string value) : type_(Type::String), string_(std::move(value)) {}
    Value(std::string_view value) : type_(Type::String), string_(value) {}

    /// Brace initialization produces an array, which is what
    /// `node["position"] = {x, y, z}` needs.
    Value(std::initializer_list<Value> values)
        : type_(Type::Array), array_(values.begin(), values.end()) {}

    /// Empty containers.
    [[nodiscard]] static Value array() {
        Value value;
        value.type_ = Type::Array;
        return value;
    }
    [[nodiscard]] static Value object() {
        Value value;
        value.type_ = Type::Object;
        return value;
    }

    // --- Type queries -----------------------------------------------------

    [[nodiscard]] Type type() const noexcept { return type_; }

    [[nodiscard]] bool is_null() const noexcept { return type_ == Type::Null; }
    [[nodiscard]] bool is_object() const noexcept { return type_ == Type::Object; }
    [[nodiscard]] bool is_array() const noexcept { return type_ == Type::Array; }
    [[nodiscard]] bool is_string() const noexcept { return type_ == Type::String; }
    [[nodiscard]] bool is_boolean() const noexcept { return type_ == Type::Boolean; }
    [[nodiscard]] bool is_number() const noexcept { return type_ == Type::Number; }

    [[nodiscard]] bool is_number_unsigned() const noexcept {
        return type_ == Type::Number && number_kind_ == NumberKind::Unsigned;
    }
    /// True for any whole number, signed or unsigned. Matches nlohmann's meaning.
    [[nodiscard]] bool is_number_integer() const noexcept {
        return type_ == Type::Number && number_kind_ != NumberKind::Real;
    }
    [[nodiscard]] bool is_number_float() const noexcept {
        return type_ == Type::Number && number_kind_ == NumberKind::Real;
    }

    // --- Object access ----------------------------------------------------

    [[nodiscard]] bool contains(std::string_view key) const;

    /// Throws type_error when this is not an object, or when the key is absent.
    [[nodiscard]] const Value& at(std::string_view key) const;

    /// Inserts a null value when the key is absent, preserving insertion order.
    /// This is the writing path: `node["name"] = value`.
    [[nodiscard]] Value& operator[](std::string_view key);

    /// Members in insertion order.
    [[nodiscard]] const std::vector<std::pair<std::string, Value>>& items() const;

    // --- Array access -----------------------------------------------------

    /// Throws type_error when this is not an array or the index is out of range.
    [[nodiscard]] const Value& operator[](std::size_t index) const;

    void push_back(Value value);

    /// Member count for an object, element count for an array, zero otherwise.
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] bool empty() const noexcept { return size() == 0; }

    // --- Extraction -------------------------------------------------------

    /// Typed extraction. Throws type_error on a mismatch or on a value that does
    /// not fit the requested type, so a narrowing conversion cannot silently wrap.
    template <typename T>
    [[nodiscard]] T get() const;

    // --- Serialization ----------------------------------------------------

    /// indent < 0 produces compact output; otherwise pretty printed with that
    /// many spaces per level.
    [[nodiscard]] std::string dump(int indent = -1) const;

    // --- Parsing ----------------------------------------------------------

    /// Parses text. allow_comments accepts // and /* */, which the symbol
    /// descriptors use to document how each anchor was derived.
    ///
    /// The signature carries the two unused parameters that nlohmann's parse takes
    /// so call sites read identically.
    [[nodiscard]] static Value parse(std::string_view text, std::nullptr_t = nullptr,
                                     bool throw_on_error = true, bool allow_comments = false);

    /// Reads the whole stream, then parses.
    [[nodiscard]] static Value parse(std::istream& stream, std::nullptr_t = nullptr,
                                     bool throw_on_error = true, bool allow_comments = false);

private:
    void RequireType(Type expected, const char* context) const;
    void DumpInto(std::string& out, int indent, int depth) const;

    Type       type_{Type::Null};
    NumberKind number_kind_{NumberKind::Unsigned};

    bool          boolean_{false};
    std::uint64_t unsigned_{0};
    std::int64_t  signed_{0};
    double        real_{0.0};
    std::string   string_;

    std::vector<Value>                             array_;
    std::vector<std::pair<std::string, Value>>     members_; ///< Insertion ordered.
};

// Names matching the library this replaced, so consumers read the same.
using json         = Value;
using ordered_json = Value;

// Explicit instantiations declared so a get<T> for an unsupported type is a link
// error rather than a silent template expansion.
template <> [[nodiscard]] bool          Value::get<bool>() const;
template <> [[nodiscard]] std::uint8_t  Value::get<std::uint8_t>() const;
template <> [[nodiscard]] std::uint16_t Value::get<std::uint16_t>() const;
template <> [[nodiscard]] std::uint32_t Value::get<std::uint32_t>() const;
template <> [[nodiscard]] std::uint64_t Value::get<std::uint64_t>() const;
template <> [[nodiscard]] std::int32_t  Value::get<std::int32_t>() const;
template <> [[nodiscard]] std::int64_t  Value::get<std::int64_t>() const;
template <> [[nodiscard]] float         Value::get<float>() const;
template <> [[nodiscard]] double        Value::get<double>() const;
template <> [[nodiscard]] std::string   Value::get<std::string>() const;
template <> [[nodiscard]] std::vector<std::string> Value::get<std::vector<std::string>>() const;
template <> [[nodiscard]] std::vector<std::size_t> Value::get<std::vector<std::size_t>>() const;
// Note: no separate specialization for int. On MSVC x64 int and std::int32_t are
// the same type, so declaring both would be a redefinition.

} // namespace fe::json
