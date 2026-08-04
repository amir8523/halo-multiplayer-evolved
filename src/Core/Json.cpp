// SPDX-License-Identifier: MIT
// MultiplayerEvolved: Core/Json.cpp
#include "Core/Json.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <format>
#include <iterator>
#include <limits>
#include <sstream>

namespace mpe::json {
namespace {

/// Hard ceiling on a parsed document. The map parser applies its own, smaller
/// limit; this one exists so even a direct call cannot be handed a gigabyte.
constexpr std::size_t kMaxDocumentBytes = 64u * 1024u * 1024u;

// ---------------------------------------------------------------------------
// Parser
// ---------------------------------------------------------------------------

class Parser {
public:
    Parser(std::string_view text, bool allow_comments)
        : text_(text), allow_comments_(allow_comments) {}

    [[nodiscard]] Value ParseDocument() {
        SkipTrivia();
        Value root = ParseValue(0);
        SkipTrivia();
        if (cursor_ != text_.size()) {
            Fail("unexpected trailing content after the top level value");
        }
        return root;
    }

private:
    [[noreturn]] void Fail(std::string_view message) const {
        throw parse_error(cursor_, std::format("JSON parse error at byte {}: {}", cursor_,
                                               message));
    }

    [[nodiscard]] bool AtEnd() const noexcept { return cursor_ >= text_.size(); }
    [[nodiscard]] char Peek() const {
        if (AtEnd()) {
            Fail("unexpected end of document");
        }
        return text_[cursor_];
    }

    void Expect(char expected) {
        if (AtEnd() || text_[cursor_] != expected) {
            Fail(std::format("expected '{}'", expected));
        }
        ++cursor_;
    }

    /// Whitespace, and comments when enabled.
    void SkipTrivia() {
        for (;;) {
            while (!AtEnd()) {
                const char c = text_[cursor_];
                if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                    ++cursor_;
                    continue;
                }
                break;
            }
            if (!allow_comments_ || AtEnd() || text_[cursor_] != '/') {
                return;
            }
            if (cursor_ + 1 >= text_.size()) {
                Fail("truncated comment");
            }
            const char kind = text_[cursor_ + 1];
            if (kind == '/') {
                cursor_ += 2;
                while (!AtEnd() && text_[cursor_] != '\n') {
                    ++cursor_;
                }
            } else if (kind == '*') {
                cursor_ += 2;
                for (;;) {
                    if (cursor_ + 1 >= text_.size()) {
                        Fail("unterminated block comment");
                    }
                    if (text_[cursor_] == '*' && text_[cursor_ + 1] == '/') {
                        cursor_ += 2;
                        break;
                    }
                    ++cursor_;
                }
            } else {
                Fail("stray '/' outside a comment");
            }
        }
    }

    [[nodiscard]] Value ParseValue(int depth) {
        if (depth > kMaxDepth) {
            Fail(std::format("nesting deeper than {} levels", kMaxDepth));
        }
        SkipTrivia();
        switch (Peek()) {
            case '{': return ParseObject(depth);
            case '[': return ParseArray(depth);
            case '"': return Value(ParseString());
            case 't': Literal("true");  return Value(true);
            case 'f': Literal("false"); return Value(false);
            case 'n': Literal("null");  return Value(nullptr);
            default:  return ParseNumber();
        }
    }

    void Literal(std::string_view expected) {
        if (text_.compare(cursor_, expected.size(), expected) != 0) {
            Fail(std::format("expected '{}'", expected));
        }
        cursor_ += expected.size();
    }

    [[nodiscard]] Value ParseObject(int depth) {
        Expect('{');
        Value result = Value::object();

        SkipTrivia();
        if (!AtEnd() && Peek() == '}') {
            ++cursor_;
            return result;
        }

        for (;;) {
            SkipTrivia();
            std::string key = ParseString();
            SkipTrivia();
            Expect(':');
            Value member = ParseValue(depth + 1);

            // A duplicate key is a document error rather than last-wins: silently
            // discarding one of two conflicting values is how a map ends up
            // different from what the author read on screen.
            if (result.contains(key)) {
                Fail(std::format("duplicate key '{}'", key));
            }
            result[key] = std::move(member);

            SkipTrivia();
            if (AtEnd()) {
                Fail("unterminated object");
            }
            if (text_[cursor_] == ',') {
                ++cursor_;
                SkipTrivia();
                // Reject a trailing comma explicitly so the message is useful.
                if (!AtEnd() && text_[cursor_] == '}') {
                    Fail("trailing comma before '}'");
                }
                continue;
            }
            if (text_[cursor_] == '}') {
                ++cursor_;
                return result;
            }
            Fail("expected ',' or '}'");
        }
    }

    [[nodiscard]] Value ParseArray(int depth) {
        Expect('[');
        Value result = Value::array();

        SkipTrivia();
        if (!AtEnd() && Peek() == ']') {
            ++cursor_;
            return result;
        }

        for (;;) {
            result.push_back(ParseValue(depth + 1));

            SkipTrivia();
            if (AtEnd()) {
                Fail("unterminated array");
            }
            if (text_[cursor_] == ',') {
                ++cursor_;
                SkipTrivia();
                if (!AtEnd() && text_[cursor_] == ']') {
                    Fail("trailing comma before ']'");
                }
                continue;
            }
            if (text_[cursor_] == ']') {
                ++cursor_;
                return result;
            }
            Fail("expected ',' or ']'");
        }
    }

    /// Appends one code point as UTF-8.
    static void AppendUtf8(std::string& out, std::uint32_t code_point) {
        if (code_point <= 0x7F) {
            out.push_back(static_cast<char>(code_point));
        } else if (code_point <= 0x7FF) {
            out.push_back(static_cast<char>(0xC0 | (code_point >> 6)));
            out.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
        } else if (code_point <= 0xFFFF) {
            out.push_back(static_cast<char>(0xE0 | (code_point >> 12)));
            out.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (code_point >> 18)));
            out.push_back(static_cast<char>(0x80 | ((code_point >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
        }
    }

    [[nodiscard]] std::uint32_t ParseHex4() {
        if (cursor_ + 4 > text_.size()) {
            Fail("truncated \\u escape");
        }
        std::uint32_t value = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = text_[cursor_++];
            std::uint32_t digit = 0;
            if (c >= '0' && c <= '9')      { digit = static_cast<std::uint32_t>(c - '0'); }
            else if (c >= 'a' && c <= 'f') { digit = static_cast<std::uint32_t>(c - 'a' + 10); }
            else if (c >= 'A' && c <= 'F') { digit = static_cast<std::uint32_t>(c - 'A' + 10); }
            else { Fail("invalid hex digit in \\u escape"); }
            value = (value << 4) | digit;
        }
        return value;
    }

    [[nodiscard]] std::string ParseString() {
        Expect('"');
        std::string result;

        for (;;) {
            if (AtEnd()) {
                Fail("unterminated string");
            }
            const char c = text_[cursor_++];

            if (c == '"') {
                return result;
            }
            if (static_cast<unsigned char>(c) < 0x20) {
                Fail("raw control character in string; use an escape");
            }
            if (c != '\\') {
                result.push_back(c);
                continue;
            }

            if (AtEnd()) {
                Fail("truncated escape sequence");
            }
            switch (const char escape = text_[cursor_++]) {
                case '"':  result.push_back('"');  break;
                case '\\': result.push_back('\\'); break;
                case '/':  result.push_back('/');  break;
                case 'b':  result.push_back('\b'); break;
                case 'f':  result.push_back('\f'); break;
                case 'n':  result.push_back('\n'); break;
                case 'r':  result.push_back('\r'); break;
                case 't':  result.push_back('\t'); break;
                case 'u': {
                    std::uint32_t code_point = ParseHex4();
                    // A high surrogate must be followed by its low surrogate.
                    if (code_point >= 0xD800 && code_point <= 0xDBFF) {
                        if (cursor_ + 2 <= text_.size() && text_[cursor_] == '\\' &&
                            text_[cursor_ + 1] == 'u') {
                            cursor_ += 2;
                            const std::uint32_t low = ParseHex4();
                            if (low < 0xDC00 || low > 0xDFFF) {
                                Fail("high surrogate not followed by a low surrogate");
                            }
                            code_point = 0x10000 + ((code_point - 0xD800) << 10) + (low - 0xDC00);
                        } else {
                            Fail("unpaired high surrogate");
                        }
                    } else if (code_point >= 0xDC00 && code_point <= 0xDFFF) {
                        Fail("unpaired low surrogate");
                    }
                    AppendUtf8(result, code_point);
                    break;
                }
                default:
                    Fail(std::format("unknown escape '\\{}'", escape));
            }
        }
    }

    [[nodiscard]] Value ParseNumber() {
        const std::size_t start = cursor_;

        if (!AtEnd() && text_[cursor_] == '-') {
            ++cursor_;
        }
        // JSON forbids a leading zero followed by more digits, and requires at
        // least one integer digit.
        if (AtEnd() || text_[cursor_] < '0' || text_[cursor_] > '9') {
            Fail("expected a number");
        }
        if (text_[cursor_] == '0') {
            ++cursor_;
        } else {
            while (!AtEnd() && text_[cursor_] >= '0' && text_[cursor_] <= '9') {
                ++cursor_;
            }
        }

        bool is_real = false;
        if (!AtEnd() && text_[cursor_] == '.') {
            is_real = true;
            ++cursor_;
            if (AtEnd() || text_[cursor_] < '0' || text_[cursor_] > '9') {
                Fail("expected a digit after the decimal point");
            }
            while (!AtEnd() && text_[cursor_] >= '0' && text_[cursor_] <= '9') {
                ++cursor_;
            }
        }
        if (!AtEnd() && (text_[cursor_] == 'e' || text_[cursor_] == 'E')) {
            is_real = true;
            ++cursor_;
            if (!AtEnd() && (text_[cursor_] == '+' || text_[cursor_] == '-')) {
                ++cursor_;
            }
            if (AtEnd() || text_[cursor_] < '0' || text_[cursor_] > '9') {
                Fail("expected a digit in the exponent");
            }
            while (!AtEnd() && text_[cursor_] >= '0' && text_[cursor_] <= '9') {
                ++cursor_;
            }
        }

        const std::string_view token = text_.substr(start, cursor_ - start);

        if (!is_real) {
            if (token.front() == '-') {
                std::int64_t value = 0;
                const auto result =
                    std::from_chars(token.data(), token.data() + token.size(), value);
                if (result.ec == std::errc{} && result.ptr == token.data() + token.size()) {
                    return Value(value);
                }
            } else {
                std::uint64_t value = 0;
                const auto result =
                    std::from_chars(token.data(), token.data() + token.size(), value);
                if (result.ec == std::errc{} && result.ptr == token.data() + token.size()) {
                    return Value(value);
                }
            }
            // Out of integer range: fall through to a double so a very large count
            // is reported as a range error by the consumer rather than a parse error.
        }

        double real = 0.0;
        const auto result = std::from_chars(token.data(), token.data() + token.size(), real);
        if (result.ec != std::errc{} || result.ptr != token.data() + token.size()) {
            Fail("malformed number");
        }
        return Value(real);
    }

    std::string_view text_;
    std::size_t      cursor_{0};
    bool             allow_comments_{false};
};

// ---------------------------------------------------------------------------
// Writer
// ---------------------------------------------------------------------------

void WriteEscapedString(std::string& out, std::string_view text) {
    out.push_back('"');
    for (const char c : text) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    out += std::format("\\u{:04x}", static_cast<unsigned char>(c));
                } else {
                    // Bytes above 0x7F pass through: the input is already UTF-8 and
                    // re-encoding it would corrupt multi byte sequences.
                    out.push_back(c);
                }
        }
    }
    out.push_back('"');
}

/// Formats a double so parsing it back yields the identical value.
///
/// Round trip exactness matters: a coordinate written and read must not drift,
/// because the canonical binary hash is taken from these values.
void WriteReal(std::string& out, double value) {
    if (!std::isfinite(value)) {
        // JSON has no representation for these. Null is the only lossless choice,
        // and the parser's own range checks reject them on the way in, so this is
        // unreachable for any document this project produces.
        out += "null";
        return;
    }

    char buffer[40];
    // Shortest representation that round trips.
    auto result = std::to_chars(buffer, buffer + sizeof(buffer), value);
    if (result.ec != std::errc{}) {
        out += "0.0";
        return;
    }
    std::string_view text(buffer, static_cast<std::size_t>(result.ptr - buffer));
    out += text;

    // A value that formats as an integer needs a decimal point, otherwise reading
    // it back would classify it as an integer and change is_number_float.
    if (text.find('.') == std::string_view::npos &&
        text.find('e') == std::string_view::npos &&
        text.find('E') == std::string_view::npos &&
        text.find("inf") == std::string_view::npos &&
        text.find("nan") == std::string_view::npos) {
        out += ".0";
    }
}

} // namespace

// ---------------------------------------------------------------------------
// Value
// ---------------------------------------------------------------------------

Value::Value(std::int64_t value) : type_(Type::Number) {
    // A non negative literal is stored as unsigned so is_number_unsigned reports
    // what the author actually wrote.
    if (value >= 0) {
        number_kind_ = NumberKind::Unsigned;
        unsigned_    = static_cast<std::uint64_t>(value);
    } else {
        number_kind_ = NumberKind::Signed;
        signed_      = value;
    }
}

void Value::RequireType(Type expected, const char* context) const {
    if (type_ != expected) {
        static constexpr const char* kNames[] = {"null",   "object", "array",
                                                 "string", "number", "boolean"};
        throw type_error(std::format("{}: value is {} but {} was required", context,
                                     kNames[static_cast<std::size_t>(type_)],
                                     kNames[static_cast<std::size_t>(expected)]));
    }
}

bool Value::contains(std::string_view key) const {
    if (type_ != Type::Object) {
        return false;
    }
    return std::any_of(members_.begin(), members_.end(),
                       [key](const auto& entry) { return entry.first == key; });
}

const Value& Value::at(std::string_view key) const {
    RequireType(Type::Object, "at(key)");
    const auto it = std::find_if(members_.begin(), members_.end(),
                                 [key](const auto& entry) { return entry.first == key; });
    if (it == members_.end()) {
        throw type_error(std::format("at(key): no member named '{}'", key));
    }
    return it->second;
}

Value& Value::operator[](std::string_view key) {
    // Assigning into a null value turns it into an object, which is what makes
    // `Value root; root["a"] = 1;` work.
    if (type_ == Type::Null) {
        type_ = Type::Object;
    }
    RequireType(Type::Object, "operator[](key)");

    const auto it = std::find_if(members_.begin(), members_.end(),
                                 [key](const auto& entry) { return entry.first == key; });
    if (it != members_.end()) {
        return it->second;
    }
    members_.emplace_back(std::string(key), Value{});
    return members_.back().second;
}

const std::vector<std::pair<std::string, Value>>& Value::items() const {
    RequireType(Type::Object, "items()");
    return members_;
}

const Value& Value::operator[](std::size_t index) const {
    RequireType(Type::Array, "operator[](index)");
    if (index >= array_.size()) {
        throw type_error(std::format("operator[](index): index {} is past the end of a {} "
                                     "element array",
                                     index, array_.size()));
    }
    return array_[index];
}

void Value::push_back(Value value) {
    if (type_ == Type::Null) {
        type_ = Type::Array;
    }
    RequireType(Type::Array, "push_back");
    array_.push_back(std::move(value));
}

std::size_t Value::size() const noexcept {
    switch (type_) {
        case Type::Object: return members_.size();
        case Type::Array:  return array_.size();
        default:           return 0;
    }
}

// --- Extraction -------------------------------------------------------------

namespace {

/// Shared integer extraction with an explicit range check, so a value that does
/// not fit the destination is an error rather than a wrap.
template <typename T>
[[nodiscard]] T ExtractUnsigned(const Value& value, Value::NumberKind kind,
                                std::uint64_t raw) {
    if (kind != Value::NumberKind::Unsigned) {
        throw type_error("get<unsigned>: value is negative or fractional");
    }
    if (raw > static_cast<std::uint64_t>(std::numeric_limits<T>::max())) {
        throw type_error(std::format("get<unsigned>: {} does not fit the destination type", raw));
    }
    (void)value;
    return static_cast<T>(raw);
}

} // namespace

template <> bool Value::get<bool>() const {
    RequireType(Type::Boolean, "get<bool>");
    return boolean_;
}

template <> std::uint64_t Value::get<std::uint64_t>() const {
    RequireType(Type::Number, "get<uint64>");
    if (number_kind_ != NumberKind::Unsigned) {
        throw type_error("get<uint64>: value is negative or fractional");
    }
    return unsigned_;
}

template <> std::uint8_t Value::get<std::uint8_t>() const {
    RequireType(Type::Number, "get<uint8>");
    return ExtractUnsigned<std::uint8_t>(*this, number_kind_, unsigned_);
}

template <> std::uint16_t Value::get<std::uint16_t>() const {
    RequireType(Type::Number, "get<uint16>");
    return ExtractUnsigned<std::uint16_t>(*this, number_kind_, unsigned_);
}

template <> std::uint32_t Value::get<std::uint32_t>() const {
    RequireType(Type::Number, "get<uint32>");
    return ExtractUnsigned<std::uint32_t>(*this, number_kind_, unsigned_);
}

template <> std::int64_t Value::get<std::int64_t>() const {
    RequireType(Type::Number, "get<int64>");
    switch (number_kind_) {
        case NumberKind::Unsigned:
            if (unsigned_ > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
                throw type_error("get<int64>: value exceeds the signed range");
            }
            return static_cast<std::int64_t>(unsigned_);
        case NumberKind::Signed:
            return signed_;
        case NumberKind::Real:
            throw type_error("get<int64>: value is fractional");
    }
    throw type_error("get<int64>: unreachable number kind");
}

template <> std::int32_t Value::get<std::int32_t>() const {
    const std::int64_t wide = get<std::int64_t>();
    if (wide < std::numeric_limits<std::int32_t>::min() ||
        wide > std::numeric_limits<std::int32_t>::max()) {
        throw type_error(std::format("get<int32>: {} does not fit", wide));
    }
    return static_cast<std::int32_t>(wide);
}

template <> double Value::get<double>() const {
    RequireType(Type::Number, "get<double>");
    switch (number_kind_) {
        case NumberKind::Unsigned: return static_cast<double>(unsigned_);
        case NumberKind::Signed:   return static_cast<double>(signed_);
        case NumberKind::Real:     return real_;
    }
    throw type_error("get<double>: unreachable number kind");
}

template <> float Value::get<float>() const {
    return static_cast<float>(get<double>());
}

template <> std::string Value::get<std::string>() const {
    RequireType(Type::String, "get<string>");
    return string_;
}

template <> std::vector<std::string> Value::get<std::vector<std::string>>() const {
    RequireType(Type::Array, "get<vector<string>>");
    std::vector<std::string> result;
    result.reserve(array_.size());
    for (const Value& element : array_) {
        result.push_back(element.get<std::string>());
    }
    return result;
}

template <> std::vector<std::size_t> Value::get<std::vector<std::size_t>>() const {
    RequireType(Type::Array, "get<vector<size_t>>");
    std::vector<std::size_t> result;
    result.reserve(array_.size());
    for (const Value& element : array_) {
        result.push_back(static_cast<std::size_t>(element.get<std::uint64_t>()));
    }
    return result;
}

// --- Serialization ----------------------------------------------------------

void Value::DumpInto(std::string& out, int indent, int depth) const {
    const bool pretty = indent >= 0;
    const auto newline_indent = [&](int level) {
        if (!pretty) {
            return;
        }
        out.push_back('\n');
        out.append(static_cast<std::size_t>(indent * level), ' ');
    };

    switch (type_) {
        case Type::Null:
            out += "null";
            return;

        case Type::Boolean:
            out += boolean_ ? "true" : "false";
            return;

        case Type::Number:
            switch (number_kind_) {
                case NumberKind::Unsigned: out += std::format("{}", unsigned_); return;
                case NumberKind::Signed:   out += std::format("{}", signed_);   return;
                case NumberKind::Real:     WriteReal(out, real_);               return;
            }
            return;

        case Type::String:
            WriteEscapedString(out, string_);
            return;

        case Type::Array: {
            if (array_.empty()) {
                out += "[]";
                return;
            }
            out.push_back('[');
            for (std::size_t i = 0; i < array_.size(); ++i) {
                if (i > 0) {
                    out.push_back(',');
                }
                newline_indent(depth + 1);
                array_[i].DumpInto(out, indent, depth + 1);
            }
            newline_indent(depth);
            out.push_back(']');
            return;
        }

        case Type::Object: {
            if (members_.empty()) {
                out += "{}";
                return;
            }
            out.push_back('{');
            for (std::size_t i = 0; i < members_.size(); ++i) {
                if (i > 0) {
                    out.push_back(',');
                }
                newline_indent(depth + 1);
                WriteEscapedString(out, members_[i].first);
                out.push_back(':');
                if (pretty) {
                    out.push_back(' ');
                }
                members_[i].second.DumpInto(out, indent, depth + 1);
            }
            newline_indent(depth);
            out.push_back('}');
            return;
        }
    }
}

std::string Value::dump(int indent) const {
    std::string out;
    // Rough preallocation, avoiding repeated growth for a large map document.
    out.reserve(1024);
    DumpInto(out, indent, 0);
    return out;
}

// --- Parsing ----------------------------------------------------------------

Value Value::parse(std::string_view text, std::nullptr_t, bool throw_on_error,
                   bool allow_comments) {
    if (text.size() > kMaxDocumentBytes) {
        if (!throw_on_error) {
            return Value{};
        }
        throw parse_error(0, std::format("document of {} bytes exceeds the {} byte ceiling",
                                         text.size(), kMaxDocumentBytes));
    }

    if (!throw_on_error) {
        try {
            Parser parser(text, allow_comments);
            return parser.ParseDocument();
        } catch (const exception&) {
            return Value{};
        }
    }

    Parser parser(text, allow_comments);
    return parser.ParseDocument();
}

Value Value::parse(std::istream& stream, std::nullptr_t, bool throw_on_error,
                   bool allow_comments) {
    std::string text;
    {
        std::ostringstream buffer;
        buffer << stream.rdbuf();
        text = buffer.str();
    }
    return parse(text, nullptr, throw_on_error, allow_comments);
}

} // namespace mpe::json
