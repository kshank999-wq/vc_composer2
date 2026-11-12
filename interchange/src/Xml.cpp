#include "vcc/interchange/Xml.h"

#include <cstdint>
#include <exception>

namespace vcc::interchange::xml {

// --- Element ------------------------------------------------------------------

const Element* Element::child(std::string_view child_name) const noexcept
{
    for (const auto& c : children)
        if (c.name == child_name) return &c;
    return nullptr;
}

Element* Element::child(std::string_view child_name) noexcept
{
    for (auto& c : children)
        if (c.name == child_name) return &c;
    return nullptr;
}

std::vector<const Element*> Element::children_named(std::string_view child_name) const
{
    std::vector<const Element*> out;
    for (const auto& c : children)
        if (c.name == child_name) out.push_back(&c);
    return out;
}

bool Element::has_attribute(std::string_view attribute_name) const noexcept
{
    for (const auto& a : attributes)
        if (a.first == attribute_name) return true;
    return false;
}

std::string Element::attribute(std::string_view attribute_name, const std::string& fallback) const
{
    for (const auto& a : attributes)
        if (a.first == attribute_name) return a.second;
    return fallback;
}

namespace {

bool is_space(char c) noexcept { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

std::string trim(std::string_view s)
{
    std::size_t b = 0, e = s.size();
    while (b < e && is_space(s[b])) ++b;
    while (e > b && is_space(s[e - 1])) --e;
    return std::string(s.substr(b, e - b));
}

}  // namespace

std::string Element::trimmed_text() const { return trim(text); }

std::string Element::child_text(std::string_view child_name, const std::string& fallback) const
{
    const Element* c = child(child_name);
    return c ? c->trimmed_text() : fallback;
}

// --- Parser -------------------------------------------------------------------

namespace {

void append_utf8(std::string& out, std::uint32_t cp)
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

bool is_name_char(char c) noexcept
{
    return !is_space(c) && c != '/' && c != '>' && c != '=' && c != '<' && c != '"' && c != '\'' && c != '\0';
}

class Parser {
public:
    explicit Parser(std::string_view doc) : s_(doc) {}

    ParseResult run()
    {
        ParseResult result;
        // UTF-8 byte order mark.
        if (s_.size() >= 3 && static_cast<unsigned char>(s_[0]) == 0xEF &&
            static_cast<unsigned char>(s_[1]) == 0xBB && static_cast<unsigned char>(s_[2]) == 0xBF)
            pos_ = 3;
        if (s_.size() >= 2 && ((static_cast<unsigned char>(s_[0]) == 0xFF && static_cast<unsigned char>(s_[1]) == 0xFE) ||
                               (static_cast<unsigned char>(s_[0]) == 0xFE && static_cast<unsigned char>(s_[1]) == 0xFF))) {
            fail("UTF-16 documents are not supported; save the file as UTF-8");
            return finish(result);
        }

        std::vector<Element*> stack;
        bool have_root = false;
        while (ok_ && pos_ < s_.size()) {
            if (s_[pos_] != '<') {
                // Character data.
                std::size_t end = s_.find('<', pos_);
                if (end == std::string_view::npos) end = s_.size();
                std::string_view raw = s_.substr(pos_, end - pos_);
                if (stack.empty()) {
                    std::size_t k = 0;
                    while (k < raw.size() && is_space(raw[k])) ++k;
                    if (k < raw.size()) {
                        advance_to(pos_ + k);
                        fail(have_root ? "text after the root element" : "text before the root element");
                        break;
                    }
                    advance_to(end);
                } else {
                    std::string decoded;
                    if (!decode(raw, decoded)) break;
                    stack.back()->text += decoded;
                    advance_to(end);
                }
                continue;
            }
            // Markup.
            if (starts("<?")) {
                if (!skip_past("?>", "processing instruction")) break;
            } else if (starts("<!--")) {
                if (!skip_past("-->", "comment")) break;
            } else if (starts("<![CDATA[")) {
                if (stack.empty()) {
                    fail("CDATA outside the root element");
                    break;
                }
                const std::size_t begin = pos_ + 9;
                const std::size_t end = s_.find("]]>", begin);
                if (end == std::string_view::npos) {
                    fail("unterminated CDATA section");
                    break;
                }
                stack.back()->text += std::string(s_.substr(begin, end - begin));
                advance_to(end + 3);
            } else if (starts("<!DOCTYPE") || starts("<!doctype")) {
                if (have_root || !stack.empty()) {
                    fail("DOCTYPE after the root element started");
                    break;
                }
                if (!skip_doctype()) break;
            } else if (starts("<!")) {
                fail("unsupported markup declaration");
                break;
            } else if (starts("</")) {
                const int tag_line = line_;
                advance_to(pos_ + 2);
                std::string name = read_name();
                if (name.empty()) {
                    fail("malformed end tag");
                    break;
                }
                skip_spaces();
                if (pos_ >= s_.size() || s_[pos_] != '>') {
                    fail("malformed end tag </" + name + ">");
                    break;
                }
                advance_to(pos_ + 1);
                if (stack.empty()) {
                    line_ = tag_line;
                    fail("end tag </" + name + "> without a matching start tag");
                    break;
                }
                if (stack.back()->name != name) {
                    line_ = tag_line;
                    fail("mismatched end tag </" + name + ">, expected </" + stack.back()->name + "> (opened at line " +
                         std::to_string(stack.back()->line) + ")");
                    break;
                }
                stack.pop_back();
            } else {
                // Start tag.
                if (stack.empty() && have_root) {
                    fail("a second root element");
                    break;
                }
                if (stack.size() >= kMaxDepth) {
                    fail("elements nested too deeply");
                    break;
                }
                Element element;
                element.line = line_;
                advance_to(pos_ + 1);
                element.name = read_name();
                if (element.name.empty()) {
                    fail("malformed start tag");
                    break;
                }
                bool self_closing = false;
                if (!read_attributes(element, self_closing)) break;
                Element* placed = nullptr;
                if (stack.empty()) {
                    result.root = std::move(element);
                    placed = &result.root;
                    have_root = true;
                } else {
                    stack.back()->children.push_back(std::move(element));
                    placed = &stack.back()->children.back();
                }
                if (!self_closing) stack.push_back(placed);
            }
        }
        if (ok_ && !stack.empty()) {
            fail("unexpected end of document: <" + stack.back()->name + "> opened at line " +
                 std::to_string(stack.back()->line) + " is not closed");
        }
        if (ok_ && !have_root) fail("no root element");
        return finish(result);
    }

private:
    static constexpr std::size_t kMaxDepth = 512;

    ParseResult& finish(ParseResult& r)
    {
        r.ok = ok_;
        if (!ok_) {
            r.error = "line " + std::to_string(error_line_) + ": " + error_;
            r.error_line = error_line_;
            r.root = Element{};
        }
        return r;
    }

    void fail(const std::string& message)
    {
        if (!ok_) return;
        ok_ = false;
        error_ = message;
        error_line_ = line_;
    }

    bool starts(std::string_view prefix) const noexcept { return s_.compare(pos_, prefix.size(), prefix) == 0; }

    void advance_to(std::size_t to)
    {
        if (to > s_.size()) to = s_.size();
        for (std::size_t i = pos_; i < to; ++i)
            if (s_[i] == '\n') ++line_;
        pos_ = to;
    }

    void skip_spaces()
    {
        while (pos_ < s_.size() && is_space(s_[pos_])) {
            if (s_[pos_] == '\n') ++line_;
            ++pos_;
        }
    }

    bool skip_past(std::string_view terminator, const char* what)
    {
        const std::size_t end = s_.find(terminator, pos_);
        if (end == std::string_view::npos) {
            fail(std::string("unterminated ") + what);
            return false;
        }
        advance_to(end + terminator.size());
        return true;
    }

    /// Skips `<!DOCTYPE ...>`, including a bracketed internal subset with
    /// quoted strings and comments in it. The external DTD is never read.
    bool skip_doctype()
    {
        advance_to(pos_ + 9);
        char quote = 0;
        int bracket = 0;
        while (pos_ < s_.size()) {
            const char c = s_[pos_];
            if (quote) {
                if (c == quote) quote = 0;
            } else if (c == '"' || c == '\'') {
                quote = c;
            } else if (bracket > 0 && starts("<!--")) {
                if (!skip_past("-->", "comment in DOCTYPE")) return false;
                continue;
            } else if (c == '[') {
                ++bracket;
            } else if (c == ']') {
                if (bracket > 0) --bracket;
            } else if (c == '>' && bracket == 0) {
                advance_to(pos_ + 1);
                return true;
            }
            advance_to(pos_ + 1);
        }
        fail("unterminated DOCTYPE");
        return false;
    }

    std::string read_name()
    {
        const std::size_t begin = pos_;
        while (pos_ < s_.size() && is_name_char(s_[pos_])) ++pos_;
        return std::string(s_.substr(begin, pos_ - begin));
    }

    bool read_attributes(Element& element, bool& self_closing)
    {
        while (true) {
            skip_spaces();
            if (pos_ >= s_.size()) {
                fail("unexpected end of document inside <" + element.name + ">");
                return false;
            }
            const char c = s_[pos_];
            if (c == '>') {
                advance_to(pos_ + 1);
                return true;
            }
            if (c == '/') {
                if (pos_ + 1 < s_.size() && s_[pos_ + 1] == '>') {
                    self_closing = true;
                    advance_to(pos_ + 2);
                    return true;
                }
                fail("malformed tag <" + element.name + ">");
                return false;
            }
            std::string name = read_name();
            if (name.empty()) {
                fail("malformed attribute in <" + element.name + ">");
                return false;
            }
            skip_spaces();
            if (pos_ >= s_.size() || s_[pos_] != '=') {
                fail("attribute '" + name + "' in <" + element.name + "> has no value");
                return false;
            }
            advance_to(pos_ + 1);
            skip_spaces();
            if (pos_ >= s_.size() || (s_[pos_] != '"' && s_[pos_] != '\'')) {
                fail("attribute '" + name + "' in <" + element.name + "> is not quoted");
                return false;
            }
            const char quote = s_[pos_];
            const std::size_t begin = pos_ + 1;
            const std::size_t end = s_.find(quote, begin);
            if (end == std::string_view::npos) {
                fail("unterminated attribute value in <" + element.name + ">");
                return false;
            }
            std::string_view raw = s_.substr(begin, end - begin);
            if (raw.find('<') != std::string_view::npos) {
                fail("'<' in the value of attribute '" + name + "'");
                return false;
            }
            // Attribute-value normalisation: literal whitespace becomes spaces
            // (character references such as &#10; are kept as written).
            std::string normalised(raw);
            for (char& ch : normalised)
                if (ch == '\r' || ch == '\t' || ch == '\n') ch = ' ';
            std::string value;
            if (!decode(normalised, value)) return false;
            for (const auto& a : element.attributes)
                if (a.first == name) {
                    fail("duplicate attribute '" + name + "' in <" + element.name + ">");
                    return false;
                }
            element.attributes.emplace_back(std::move(name), std::move(value));
            advance_to(end + 1);
        }
    }

    /// Decodes entity and character references. An entity this parser does
    /// not know (one a DTD would define) is kept literally rather than
    /// rejected, because MusicXML never needs one and a lost "&foo;" in a
    /// title is better than a refused file.
    bool decode(std::string_view raw, std::string& out)
    {
        out.reserve(out.size() + raw.size());
        int line = line_;
        for (std::size_t i = 0; i < raw.size(); ++i) {
            const char c = raw[i];
            if (c == '\n') ++line;
            if (c == '\r') {
                // Line-end normalisation: CRLF and lone CR become LF.
                if (i + 1 < raw.size() && raw[i + 1] == '\n') continue;
                out += '\n';
                continue;
            }
            if (c != '&') {
                out += c;
                continue;
            }
            const std::size_t semi = raw.find(';', i);
            if (semi == std::string_view::npos || semi - i > 12) {
                line_ = line;
                fail("unescaped '&' (write &amp;)");
                return false;
            }
            std::string_view ent = raw.substr(i + 1, semi - i - 1);
            if (ent == "lt") out += '<';
            else if (ent == "gt") out += '>';
            else if (ent == "amp") out += '&';
            else if (ent == "quot") out += '"';
            else if (ent == "apos") out += '\'';
            else if (!ent.empty() && ent[0] == '#') {
                std::uint32_t cp = 0;
                const bool hex = ent.size() > 1 && (ent[1] == 'x' || ent[1] == 'X');
                std::size_t k = hex ? 2 : 1;
                if (k >= ent.size()) {
                    line_ = line;
                    fail("malformed character reference &" + std::string(ent) + ";");
                    return false;
                }
                for (; k < ent.size(); ++k) {
                    const char d = ent[k];
                    int v = -1;
                    if (d >= '0' && d <= '9') v = d - '0';
                    else if (hex && d >= 'a' && d <= 'f') v = d - 'a' + 10;
                    else if (hex && d >= 'A' && d <= 'F') v = d - 'A' + 10;
                    if (v < 0 || cp > 0x10FFFF) {
                        line_ = line;
                        fail("malformed character reference &" + std::string(ent) + ";");
                        return false;
                    }
                    cp = cp * (hex ? 16u : 10u) + static_cast<std::uint32_t>(v);
                }
                if (cp == 0 || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
                    line_ = line;
                    fail("character reference &" + std::string(ent) + "; is not a valid character");
                    return false;
                }
                append_utf8(out, cp);
            } else {
                out.append(raw.substr(i, semi - i + 1));
            }
            i = semi;
        }
        return true;
    }

    std::string_view s_;
    std::size_t pos_ = 0;
    int line_ = 1;
    bool ok_ = true;
    std::string error_;
    int error_line_ = 0;
};

}  // namespace

ParseResult parse(std::string_view document)
{
    try {
        return Parser(document).run();
    } catch (const std::exception& e) {
        ParseResult r;
        r.error = std::string("line 0: ") + e.what();
        return r;
    }
}

// --- Writer -------------------------------------------------------------------

std::string escape(std::string_view text, bool attribute)
{
    std::string out;
    out.reserve(text.size());
    for (char c : text) {
        const auto u = static_cast<unsigned char>(c);
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"':
                if (attribute) out += "&quot;";
                else out += c;
                break;
            case '\n':
            case '\t':
            case '\r':
                if (attribute) out += "&#" + std::to_string(u) + ";";
                else out += c;
                break;
            default:
                if (u >= 0x20) out += c;
        }
    }
    return out;
}

void Writer::declaration() { out_ += "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"no\"?>\n"; }

void Writer::raw_line(std::string_view line)
{
    out_.append(line);
    out_ += '\n';
}

void Writer::start_line() { out_.append(open_.size() * static_cast<std::size_t>(indent_), ' '); }

void Writer::write_start(std::string_view name, const Attributes& attributes)
{
    start_line();
    out_ += '<';
    out_.append(name);
    for (const auto& a : attributes) {
        out_ += ' ';
        out_ += a.first;
        out_ += "=\"";
        out_ += escape(a.second, true);
        out_ += '"';
    }
}

void Writer::open(std::string_view name, const Attributes& attributes)
{
    write_start(name, attributes);
    out_ += ">\n";
    open_.emplace_back(name);
}

void Writer::close()
{
    if (open_.empty()) return;
    std::string name = std::move(open_.back());
    open_.pop_back();
    start_line();
    out_ += "</" + name + ">\n";
}

void Writer::leaf(std::string_view name, std::string_view text, const Attributes& attributes)
{
    write_start(name, attributes);
    out_ += '>';
    out_ += escape(text);
    out_ += "</";
    out_.append(name);
    out_ += ">\n";
}

void Writer::empty(std::string_view name, const Attributes& attributes)
{
    write_start(name, attributes);
    out_ += "/>\n";
}

std::string Writer::finish()
{
    while (!open_.empty()) close();
    return std::move(out_);
}

}  // namespace vcc::interchange::xml
