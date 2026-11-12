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
    std::string to_string() const;
};

/// Parses a complete JSON document. Trailing non-whitespace is an error.
/// Nesting deeper than 256 levels is refused, so a hostile file cannot
/// exhaust the stack.
std::optional<Value> parse(const std::string& text, ParseError* error = nullptr);

struct WriteOptions {
    int indent = 2;          ///< 0 writes compact single-line JSON.
    bool sort_keys = false;  ///< Normally insertion order is kept.
};

std::string write(const Value& value, const WriteOptions& options = {});

}  // namespace vcc::json
