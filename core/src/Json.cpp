#include "vcc/core/Json.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace vcc::json {

// --- Object -----------------------------------------------------------------

Object::Object(std::initializer_list<Member> members)
{
    for (const auto& m : members) set(m.first, m.second);
}

Value* Object::find(const std::string& key) noexcept
{
    for (auto& m : members_)
        if (m.first == key) return &m.second;
    return nullptr;
}

const Value* Object::find(const std::string& key) const noexcept
{
    for (const auto& m : members_)
        if (m.first == key) return &m.second;
    return nullptr;
}

Value& Object::set(const std::string& key, Value value)
{
    if (Value* existing = find(key)) {
        *existing = std::move(value);
        return *existing;
    }
    members_.emplace_back(key, std::move(value));
    return members_.back().second;
}

Value& Object::operator[](const std::string& key)
{
    if (Value* existing = find(key)) return *existing;
    members_.emplace_back(key, Value{});
    return members_.back().second;
}

bool Object::erase(const std::string& key)
{
    auto it = std::find_if(members_.begin(), members_.end(),
                           [&](const Member& m) { return m.first == key; });
    if (it == members_.end()) return false;
    members_.erase(it);
    return true;
}

// --- Value ------------------------------------------------------------------

Value::Value(Array a) : type_(Type::Array), array_(std::make_unique<Array>(std::move(a))) {}
Value::Value(Object o) : type_(Type::Object), object_(std::make_unique<Object>(std::move(o))) {}

Value::Value(const Value& other)
    : type_(other.type_), bool_(other.bool_), int_(other.int_), double_(other.double_),
      string_(other.string_)
{
    if (other.array_) array_ = std::make_unique<Array>(*other.array_);
    if (other.object_) object_ = std::make_unique<Object>(*other.object_);
}

Value& Value::operator=(const Value& other)
{
    if (this != &other) {
        Value copy(other);
        *this = std::move(copy);
    }
    return *this;
}

bool Value::as_bool(bool fallback) const noexcept { return type_ == Type::Bool ? bool_ : fallback; }

std::int64_t Value::as_int(std::int64_t fallback) const noexcept
{
    if (type_ == Type::Int) return int_;
    // A double rounds to the nearest integer, as the original library did.
    if (type_ == Type::Double && std::isfinite(double_) && std::fabs(double_) < 9.0e18)
        return static_cast<std::int64_t>(std::llround(double_));
    return fallback;
}

double Value::as_double(double fallback) const noexcept
{
    if (type_ == Type::Double) return double_;
    if (type_ == Type::Int) return static_cast<double>(int_);
    return fallback;
}

const std::string& Value::as_string() const noexcept
{
    static const std::string empty;
    return type_ == Type::String ? string_ : empty;
}

const Array& Value::as_array() const noexcept
{
    static const Array empty;
    return (type_ == Type::Array && array_) ? *array_ : empty;
}

const Object& Value::as_object() const noexcept
{
    static const Object empty;
    return (type_ == Type::Object && object_) ? *object_ : empty;
}

Array& Value::array()
{
    if (type_ != Type::Array) *this = Value(Array{});
    return *array_;
}

Object& Value::object()
{
    if (type_ != Type::Object) *this = Value(Object{});
    return *object_;
}

const Value* Value::get(const std::string& key) const noexcept
{
    return type_ == Type::Object && object_ ? object_->find(key) : nullptr;
}

std::string Value::get_string(const std::string& key, const std::string& fallback) const
{
    const Value* v = get(key);
    return v && v->is_string() ? v->string_ : fallback;
}

std::int64_t Value::get_int(const std::string& key, std::int64_t fallback) const noexcept
{
    const Value* v = get(key);
    return v ? v->as_int(fallback) : fallback;
}

double Value::get_double(const std::string& key, double fallback) const noexcept
{
    const Value* v = get(key);
    return v ? v->as_double(fallback) : fallback;
}

bool Value::get_bool(const std::string& key, bool fallback) const noexcept
{
    const Value* v = get(key);
    return v ? v->as_bool(fallback) : fallback;
}

std::vector<std::string> Value::get_string_array(const std::string& key) const
{
    std::vector<std::string> out;
    if (const Value* v = get(key))
        for (const auto& item : v->as_array())
            if (item.is_string()) out.push_back(item.as_string());
    return out;
}

const char* to_string(Kind kind) noexcept
{
    switch (kind) {
        case Kind::Null: return "null";
        case Kind::Bool: return "bool";
        case Kind::Number: return "number";
        case Kind::String: return "string";
        case Kind::Array: return "array";
        case Kind::Object: return "object";
    }
    return "null";
}

Value Value::make_number(double d)
{
    if (std::isfinite(d) && std::floor(d) == d && std::fabs(d) <= 9007199254740992.0 && !(d == 0.0 && std::signbit(d)))
        return Value(static_cast<long long>(d));
    return Value(d);
}

Kind Value::kind() const noexcept
{
    switch (type_) {
        case Type::Null: return Kind::Null;
        case Type::Bool: return Kind::Bool;
        case Type::Int:
        case Type::Double: return Kind::Number;
        case Type::String: return Kind::String;
        case Type::Array: return Kind::Array;
        case Type::Object: return Kind::Object;
    }
    return Kind::Null;
}

std::string Value::as_string_or(const std::string& fallback) const
{
    return type_ == Type::String ? string_ : fallback;
}

std::size_t Value::size() const noexcept
{
    if (type_ == Type::Array) return as_array().size();
    if (type_ == Type::Object) return as_object().size();
    return 0;
}

namespace {
const Value& null_value() noexcept
{
    static const Value v;
    return v;
}
}  // namespace

const Value& Value::at(std::size_t index) const noexcept
{
    const Array& a = as_array();
    return index < a.size() ? a[index] : null_value();
}

const Value& Value::operator[](const std::string& key) const noexcept
{
    const Value* v = get(key);
    return v ? *v : null_value();
}

bool Value::remove(const std::string& key)
{
    if (type_ != Type::Object) return false;
    return object_->erase(key);
}

std::vector<std::string> Value::keys() const
{
    std::vector<std::string> out;
    for (const auto& m : as_object()) out.push_back(m.first);
    return out;
}

std::string Value::to_text(int indent) const
{
    std::string s = write(*this, WriteOptions{indent, false});
    if (indent > 0 && !s.empty() && s.back() == '\n') s.pop_back();
    return s;
}

bool operator==(const Value& a, const Value& b) noexcept
{
    if (a.is_number() && b.is_number()) {
        if (a.type_ == Type::Int && b.type_ == Type::Int) return a.int_ == b.int_;
        return a.as_double() == b.as_double();
    }
    if (a.type_ != b.type_) return false;
    switch (a.type_) {
        case Type::Null: return true;
        case Type::Bool: return a.bool_ == b.bool_;
        case Type::String: return a.string_ == b.string_;
        case Type::Array: return a.as_array() == b.as_array();
        case Type::Object: {
            const Object& x = a.as_object();
            const Object& y = b.as_object();
            if (x.size() != y.size()) return false;
            for (const auto& m : x) {
                const Value* other = y.find(m.first);
                if (!other || !(*other == m.second)) return false;
            }
            return true;
        }
        default: return false;
    }
}

// --- Parser -----------------------------------------------------------------

std::string ParseError::to_string() const
{
    return "line " + std::to_string(line) + ", column " + std::to_string(column) + ": " + message;
}

namespace {

class Parser {
public:
    explicit Parser(const std::string& text) : s_(text) {}

    std::optional<Value> run(ParseError* error)
    {
        skip_ws();
        Value v;
        if (!parse_value(v, 0)) return fail(error);
        skip_ws();
        if (pos_ != s_.size()) {
            set_error("unexpected trailing characters");
            return fail(error);
        }
        return v;
    }

private:
    std::optional<Value> fail(ParseError* error)
    {
        if (error) {
            error->message = message_;
            error->failed = true;
            error->offset = err_pos_;
            error->line = 1;
            error->column = 1;
            for (std::size_t i = 0; i < err_pos_ && i < s_.size(); ++i) {
                if (s_[i] == '\n') {
                    ++error->line;
                    error->column = 1;
                } else {
                    ++error->column;
                }
            }
        }
        return std::nullopt;
    }

    void set_error(const std::string& m)
    {
        if (message_.empty()) {
            message_ = m;
            err_pos_ = pos_;
        }
    }

    void skip_ws()
    {
        while (pos_ < s_.size() &&
               (s_[pos_] == ' ' || s_[pos_] == '\t' || s_[pos_] == '\n' || s_[pos_] == '\r'))
            ++pos_;
    }

    bool literal(const char* word)
    {
        std::size_t n = std::strlen(word);
        if (s_.compare(pos_, n, word) != 0) {
            set_error("invalid literal");
            return false;
        }
        pos_ += n;
        return true;
    }

    bool parse_value(Value& out, int depth)
    {
        if (depth > kMaxDepth) {
            set_error("nesting too deep");
            return false;
        }
        if (pos_ >= s_.size()) {
            set_error("unexpected end of input");
            return false;
        }
        char c = s_[pos_];
        switch (c) {
            case '{': return parse_object(out, depth);
            case '[': return parse_array(out, depth);
            case '"': {
                std::string str;
                if (!parse_string(str)) return false;
                out = Value(std::move(str));
                return true;
            }
            case 't': if (!literal("true")) return false; out = Value(true); return true;
            case 'f': if (!literal("false")) return false; out = Value(false); return true;
            case 'n': if (!literal("null")) return false; out = Value(); return true;
            default:
                if (c == '-' || (c >= '0' && c <= '9')) return parse_number(out);
                set_error(std::string("unexpected character '") + c + "'");
                return false;
        }
    }

    bool parse_number(Value& out)
    {
        std::size_t start = pos_;
        bool is_integer = true;
        if (s_[pos_] == '-') ++pos_;
        if (pos_ >= s_.size() || !(s_[pos_] >= '0' && s_[pos_] <= '9')) {
            set_error("invalid number");
            return false;
        }
        if (s_[pos_] == '0') {
            ++pos_;
        } else {
            while (pos_ < s_.size() && s_[pos_] >= '0' && s_[pos_] <= '9') ++pos_;
        }
        if (pos_ < s_.size() && s_[pos_] == '.') {
            is_integer = false;
            ++pos_;
            if (pos_ >= s_.size() || !(s_[pos_] >= '0' && s_[pos_] <= '9')) {
                set_error("invalid fraction");
                return false;
            }
            while (pos_ < s_.size() && s_[pos_] >= '0' && s_[pos_] <= '9') ++pos_;
        }
        if (pos_ < s_.size() && (s_[pos_] == 'e' || s_[pos_] == 'E')) {
            is_integer = false;
            ++pos_;
            if (pos_ < s_.size() && (s_[pos_] == '+' || s_[pos_] == '-')) ++pos_;
            if (pos_ >= s_.size() || !(s_[pos_] >= '0' && s_[pos_] <= '9')) {
                set_error("invalid exponent");
                return false;
            }
            while (pos_ < s_.size() && s_[pos_] >= '0' && s_[pos_] <= '9') ++pos_;
        }
        std::string token = s_.substr(start, pos_ - start);
        if (is_integer) {
            errno = 0;
            char* end = nullptr;
            long long v = std::strtoll(token.c_str(), &end, 10);
            if (errno == 0 && end && *end == '\0') {
                out = Value(v);
                return true;
            }
        }
        out = Value(std::strtod(token.c_str(), nullptr));
        return true;
    }

    static void append_utf8(std::string& out, std::uint32_t cp)
    {
        if (cp < 0x80) {
            out += static_cast<char>(cp);
        } else if (cp < 0x800) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }

    bool parse_hex4(std::uint32_t& out)
    {
        if (pos_ + 4 > s_.size()) {
            set_error("truncated \\u escape");
            return false;
        }
        out = 0;
        for (int i = 0; i < 4; ++i) {
            char c = s_[pos_++];
            out <<= 4;
            if (c >= '0' && c <= '9') out |= static_cast<std::uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f') out |= static_cast<std::uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') out |= static_cast<std::uint32_t>(c - 'A' + 10);
            else {
                set_error("invalid hex digit in \\u escape");
                return false;
            }
        }
        return true;
    }

    bool parse_string(std::string& out)
    {
        ++pos_;  // opening quote
        while (true) {
            if (pos_ >= s_.size()) {
                set_error("unterminated string");
                return false;
            }
            char c = s_[pos_++];
            if (c == '"') return true;
            if (static_cast<unsigned char>(c) < 0x20) {
                --pos_;
                set_error("control character in string");
                return false;
            }
            if (c != '\\') {
                out += c;
                continue;
            }
            if (pos_ >= s_.size()) {
                set_error("unterminated escape");
                return false;
            }
            char e = s_[pos_++];
            switch (e) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u': {
                    std::uint32_t cp = 0;
                    if (!parse_hex4(cp)) return false;
                    if (cp >= 0xD800 && cp <= 0xDBFF) {
                        if (pos_ + 2 <= s_.size() && s_[pos_] == '\\' && s_[pos_ + 1] == 'u') {
                            pos_ += 2;
                            std::uint32_t lo = 0;
                            if (!parse_hex4(lo)) return false;
                            if (lo < 0xDC00 || lo > 0xDFFF) {
                                set_error("invalid surrogate pair");
                                return false;
                            }
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        } else {
                            set_error("lone high surrogate");
                            return false;
                        }
                    } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                        set_error("lone low surrogate");
                        return false;
                    }
                    append_utf8(out, cp);
                    break;
                }
                default:
                    --pos_;
                    set_error("invalid escape");
                    return false;
            }
        }
    }

    bool parse_array(Value& out, int depth)
    {
        ++pos_;
        Array arr;
        skip_ws();
        if (pos_ < s_.size() && s_[pos_] == ']') {
            ++pos_;
            out = Value(std::move(arr));
            return true;
        }
        while (true) {
            skip_ws();
            Value item;
            if (!parse_value(item, depth + 1)) return false;
            arr.push_back(std::move(item));
            skip_ws();
            if (pos_ >= s_.size()) {
                set_error("unterminated array");
                return false;
            }
            if (s_[pos_] == ',') { ++pos_; continue; }
            if (s_[pos_] == ']') { ++pos_; break; }
            set_error("expected ',' or ']'");
            return false;
        }
        out = Value(std::move(arr));
        return true;
    }

    bool parse_object(Value& out, int depth)
    {
        ++pos_;
        Object obj;
        skip_ws();
        if (pos_ < s_.size() && s_[pos_] == '}') {
            ++pos_;
            out = Value(std::move(obj));
            return true;
        }
        while (true) {
            skip_ws();
            if (pos_ >= s_.size() || s_[pos_] != '"') {
                set_error("expected member name");
                return false;
            }
            std::string key;
            if (!parse_string(key)) return false;
            skip_ws();
            if (pos_ >= s_.size() || s_[pos_] != ':') {
                set_error("expected ':'");
                return false;
            }
            ++pos_;
            skip_ws();
            Value v;
            if (!parse_value(v, depth + 1)) return false;
            if (obj.contains(key)) {
                set_error("duplicate member '" + key + "'");
                return false;
            }
            obj.set(key, std::move(v));
            skip_ws();
            if (pos_ >= s_.size()) {
                set_error("unterminated object");
                return false;
            }
            if (s_[pos_] == ',') { ++pos_; continue; }
            if (s_[pos_] == '}') { ++pos_; break; }
            set_error("expected ',' or '}'");
            return false;
        }
        out = Value(std::move(obj));
        return true;
    }

    const std::string& s_;
    std::size_t pos_ = 0;
    std::string message_;
    std::size_t err_pos_ = 0;
};

void write_string(std::string& out, const std::string& s)
{
    out += '"';
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04x", c);
                    out += buf;
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    out += '"';
}

void write_double(std::string& out, double d)
{
    if (!std::isfinite(d)) {
        out += "null";  // JSON has no NaN/Inf.
        return;
    }
    // Shortest representation that round-trips.
    char buf[32];
    for (int precision = 6; precision <= 17; ++precision) {
        std::snprintf(buf, sizeof buf, "%.*g", precision, d);
        if (std::strtod(buf, nullptr) == d) break;
    }
    std::string s = buf;
    // Keep it recognisably a double so a re-read gives a double back.
    if (s.find_first_of(".eEn") == std::string::npos) s += ".0";
    out += s;
}

void write_value(std::string& out, const Value& v, const WriteOptions& opt, int level)
{
    auto newline = [&](int lvl) {
        if (opt.indent <= 0) return;
        out += '\n';
        out.append(static_cast<std::size_t>(lvl * opt.indent), ' ');
    };
    switch (v.type()) {
        case Type::Null: out += "null"; break;
        case Type::Bool: out += v.as_bool() ? "true" : "false"; break;
        case Type::Int: out += std::to_string(v.as_int()); break;
        case Type::Double: write_double(out, v.as_double()); break;
        case Type::String: write_string(out, v.as_string()); break;
        case Type::Array: {
            const Array& a = v.as_array();
            if (a.empty()) { out += "[]"; break; }
            // Arrays of scalars stay on one line; they are ranges, tags, pairs.
            bool scalar = std::all_of(a.begin(), a.end(), [](const Value& x) {
                return !x.is_array() && !x.is_object();
            });
            out += '[';
            for (std::size_t i = 0; i < a.size(); ++i) {
                if (i) out += scalar && opt.indent > 0 ? ", " : ",";
                if (!scalar) newline(level + 1);
                write_value(out, a[i], opt, level + 1);
            }
            if (!scalar) newline(level);
            out += ']';
            break;
        }
        case Type::Object: {
            const Object& o = v.as_object();
            if (o.empty()) { out += "{}"; break; }
            std::vector<const Member*> members;
            for (const auto& m : o) members.push_back(&m);
            if (opt.sort_keys)
                std::sort(members.begin(), members.end(),
                          [](const Member* a, const Member* b) { return a->first < b->first; });
            out += '{';
            for (std::size_t i = 0; i < members.size(); ++i) {
                if (i) out += ',';
                newline(level + 1);
                write_string(out, members[i]->first);
                out += opt.indent > 0 ? ": " : ":";
                write_value(out, members[i]->second, opt, level + 1);
            }
            newline(level);
            out += '}';
            break;
        }
    }
}

}  // namespace

Value parse_value(const std::string& text, ParseError& error)
{
    error = ParseError{};
    auto v = parse(text, &error);
    return v ? std::move(*v) : Value();
}

Value parse_value(const std::string& text)
{
    ParseError ignored;
    return parse_value(text, ignored);
}

std::string escape(const std::string& text)
{
    std::string quoted;
    write_string(quoted, text);
    return quoted.substr(1, quoted.size() - 2);
}

std::optional<Value> parse(const std::string& text, ParseError* error)
{
    // Tolerate a UTF-8 byte-order mark written by some Windows editors.
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
        static_cast<unsigned char>(text[1]) == 0xBB && static_cast<unsigned char>(text[2]) == 0xBF) {
        std::string stripped = text.substr(3);
        return Parser(stripped).run(error);
    }
    return Parser(text).run(error);
}

std::string write(const Value& value, const WriteOptions& options)
{
    std::string out;
    write_value(out, value, options, 0);
    if (options.indent > 0) out += '\n';
    return out;
}

}  // namespace vcc::json
