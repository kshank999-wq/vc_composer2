// JSON value, parser and writer.
//
// Content packs, project files and template exports are all JSON (Templates
// Supplement §8; Main Spec §17 "schema versioning and atomic saves"). This is
// a small, dependency-free implementation with two properties the project
// relies on:
//
//   * Object members keep their insertion order, so a saved project diffs
//     cleanly and a re-saved file is byte-identical when nothing changed.
//   * Integers survive exactly. A tick position or a 64-bit id read back from
//     disk is the integer that was written, never a rounded double.
//
// It also carries the API of the original VC Composer's JSON library
// (make_*, set, push_back, at, size, has, operator[], to_text, Kind,
// parse_value), so the ported notation score file reads and writes exactly
// as it did. There is one JSON implementation in the product, not two.
#pragma once

#include <cstdint>
#include <initializer_list>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace vcc::json {

enum class Type : std::uint8_t { Null, Bool, Int, Double, String, Array, Object };

/// The original library's coarser kinds: integers and doubles are both Number.
enum class Kind : std::uint8_t { Null = 0, Bool, Number, String, Array, Object };
const char* to_string(Kind kind) noexcept;

/// Nesting deeper than this is refused, so a hostile file cannot exhaust the
/// stack. Real documents (projects, scores, content) stay under ten levels.
constexpr int kMaxDepth = 64;

class Value;
using Array = std::vector<Value>;
using Member = std::pair<std::string, Value>;

/// An ordered JSON object. Lookup is linear; objects in this project are
/// small (tens of members), and order preservation matters more.
class Object {
public:
    Object() = default;
    Object(std::initializer_list<Member> members);

    Value* find(const std::string& key) noexcept;
    const Value* find(const std::string& key) const noexcept;
    bool contains(const std::string& key) const noexcept { return find(key) != nullptr; }

    /// Inserts or replaces, keeping the original position on replace.
    Value& set(const std::string& key, Value value);
    Value& operator[](const std::string& key);
    bool erase(const std::string& key);

    std::size_t size() const noexcept { return members_.size(); }
    bool empty() const noexcept { return members_.empty(); }
    auto begin() const noexcept { return members_.begin(); }
    auto end() const noexcept { return members_.end(); }
    auto begin() noexcept { return members_.begin(); }
    auto end() noexcept { return members_.end(); }

private:
    std::vector<Member> members_;
};

class Value {
public:
    Value() noexcept = default;
    Value(std::nullptr_t) noexcept {}
    Value(bool b) noexcept : type_(Type::Bool), bool_(b) {}
    Value(int i) noexcept : type_(Type::Int), int_(i) {}
    Value(long i) noexcept : type_(Type::Int), int_(i) {}
    Value(long long i) noexcept : type_(Type::Int), int_(i) {}
    Value(unsigned i) noexcept : type_(Type::Int), int_(i) {}
    Value(unsigned long i) noexcept : type_(Type::Int), int_(static_cast<std::int64_t>(i)) {}
    Value(unsigned long long i) noexcept : type_(Type::Int), int_(static_cast<std::int64_t>(i)) {}
    Value(double d) noexcept : type_(Type::Double), double_(d) {}
    Value(const char* s) : type_(Type::String), string_(s) {}
    Value(std::string s) : type_(Type::String), string_(std::move(s)) {}
    Value(Array a);
    Value(Object o);

    Value(const Value& other);
    Value(Value&& other) noexcept = default;
    Value& operator=(const Value& other);
    Value& operator=(Value&& other) noexcept = default;
    ~Value() = default;

    Type type() const noexcept { return type_; }
    bool is_null() const noexcept { return type_ == Type::Null; }
    bool is_bool() const noexcept { return type_ == Type::Bool; }
    bool is_int() const noexcept { return type_ == Type::Int; }
    bool is_number() const noexcept { return type_ == Type::Int || type_ == Type::Double; }
    bool is_string() const noexcept { return type_ == Type::String; }
    bool is_array() const noexcept { return type_ == Type::Array; }
    bool is_object() const noexcept { return type_ == Type::Object; }

    // Checked accessors: return the fallback when the type does not match.
    bool as_bool(bool fallback = false) const noexcept;
    std::int64_t as_int(std::int64_t fallback = 0) const noexcept;
    double as_double(double fallback = 0.0) const noexcept;
    const std::string& as_string() const noexcept;  ///< Empty string when not a string.

    // Container access. Calling these on the wrong type returns an empty
    // static container (const) or converts this value (non-const).
    const Array& as_array() const noexcept;
    const Object& as_object() const noexcept;
    Array& array();
    Object& object();

    /// Member lookup on an object; nullptr when absent or not an object.
    const Value* get(const std::string& key) const noexcept;

    // Convenience typed member getters with defaults.
    std::string get_string(const std::string& key, const std::string& fallback = {}) const;
    std::int64_t get_int(const std::string& key, std::int64_t fallback = 0) const noexcept;
    double get_double(const std::string& key, double fallback = 0.0) const noexcept;
    bool get_bool(const std::string& key, bool fallback = false) const noexcept;
    std::vector<std::string> get_string_array(const std::string& key) const;

    // --- Original VC Composer API -------------------------------------------
    static Value make_null() { return Value(); }
    static Value make_bool(bool b) { return Value(b); }
    /// Integral values within 2^53 are stored as integers, so 3.0 writes as
    /// "3" - the original library's output, which saved scores rely on.
    static Value make_number(double d);
    static Value make_string(std::string s) { return Value(std::move(s)); }
    static Value make_array() { return Value(Array{}); }
    static Value make_object() { return Value(Object{}); }

    Kind kind() const noexcept;
    double as_number(double fallback = 0.0) const noexcept { return as_double(fallback); }
    std::string as_string_or(const std::string& fallback) const;
    /// Elements of an array or members of an object; 0 otherwise.
    std::size_t size() const noexcept;
    /// Array element, or a null value when out of range or not an array.
    const Value& at(std::size_t index) const noexcept;
    /// Member, or a null value when absent - safe to chain: v["a"]["b"].
    const Value& operator[](const std::string& key) const noexcept;
    bool has(const std::string& key) const noexcept { return get(key) != nullptr; }
    void push_back(Value value) { array().push_back(std::move(value)); }
    void set(const std::string& key, Value value) { object().set(key, std::move(value)); }
    /// False when absent or not an object (the value is left as it was).
    bool remove(const std::string& key);
    std::vector<std::string> keys() const;
    /// indent 0 = compact single line.
    std::string to_text(int indent = 0) const;

    friend bool operator==(const Value& a, const Value& b) noexcept;
    friend bool operator!=(const Value& a, const Value& b) noexcept { return !(a == b); }

private:
    Type type_ = Type::Null;
    bool bool_ = false;
    std::int64_t int_ = 0;
    double double_ = 0.0;
    std::string string_;
    std::unique_ptr<Array> array_;
    std::unique_ptr<Object> object_;
};

struct ParseError {
    std::string message;
    std::size_t line = 0;    ///< 1-based.
    std::size_t column = 0;  ///< 1-based, in bytes.
    bool failed = false;
    std::size_t offset = 0;  ///< Byte offset where parsing gave up.
    std::string to_string() const;
};

/// Parses a complete JSON document. Trailing non-whitespace is an error.
/// Nesting deeper than kMaxDepth levels is refused.
std::optional<Value> parse(const std::string& text, ParseError* error = nullptr);

/// The original library's form: a null Value on failure, with `error.failed`.
Value parse_value(const std::string& text, ParseError& error);
Value parse_value(const std::string& text);

/// The text of a JSON string literal for `text`, without the quotes.
std::string escape(const std::string& text);

struct WriteOptions {
    int indent = 2;          ///< 0 writes compact single-line JSON.
    bool sort_keys = false;  ///< Normally insertion order is kept.
};

std::string write(const Value& value, const WriteOptions& options = {});

}  // namespace vcc::json
