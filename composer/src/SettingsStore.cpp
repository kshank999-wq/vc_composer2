#include "vcc/composer/SettingsStore.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace vcc::composer {
namespace {

std::string trim(const std::string& text)
{
    const std::size_t first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
        return {};
    const std::size_t last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
}

}  // namespace

std::string escape_value(const std::string& value)
{
    // Backslash first, or the escapes introduced below would be re-escaped.
    std::string result;
    result.reserve(value.size());

    for (const char character : value) {
        switch (character) {
            case '\\': result += "\\\\"; break;
            case '\n': result += "\\n"; break;
            case '\r': result += "\\r"; break;
            case '\t': result += "\\t"; break;
            default:   result += character; break;
        }
    }

    // Leading and trailing spaces would be eaten by the parser's trim, so
    // they are encoded explicitly.
    if (!result.empty() && (result.front() == ' ' || result.back() == ' '))
        result = "\\s" + result + "\\s";

    return result;
}

std::string unescape_value(const std::string& value)
{
    std::string working = value;

    // Strip the whitespace guards, innermost last.
    if (working.size() >= 4 && working.compare(0, 2, "\\s") == 0
        && working.compare(working.size() - 2, 2, "\\s") == 0) {
        working = working.substr(2, working.size() - 4);
    }

    std::string result;
    result.reserve(working.size());

    for (std::size_t index = 0; index < working.size(); ++index) {
        if (working[index] != '\\' || index + 1 >= working.size()) {
            result += working[index];
            continue;
        }

        switch (working[index + 1]) {
            case '\\': result += '\\'; ++index; break;
            case 'n':  result += '\n'; ++index; break;
            case 'r':  result += '\r'; ++index; break;
            case 't':  result += '\t'; ++index; break;
            default:   result += working[index]; break;
        }
    }

    return result;
}

void SettingsSection::set(const std::string& key, const std::string& value)
{
    if (key.empty())
        return;
    values_[key] = value;
}

void SettingsSection::set_int(const std::string& key, std::int64_t value)
{
    set(key, std::to_string(value));
}

void SettingsSection::set_double(const std::string& key, double value)
{
    // 17 significant digits round-trips a double exactly.
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.17g", value);
    set(key, buffer);
}

void SettingsSection::set_bool(const std::string& key, bool value)
{
    set(key, value ? "true" : "false");
}

std::string SettingsSection::get(const std::string& key, const std::string& fallback) const
{
    const auto found = values_.find(key);
    return found == values_.end() ? fallback : found->second;
}

std::int64_t SettingsSection::get_int(const std::string& key, std::int64_t fallback) const
{
    const auto found = values_.find(key);
    if (found == values_.end())
        return fallback;

    // strtoll rather than stoll: a malformed value must not throw out of a
    // settings read.
    const char* text = found->second.c_str();
    char* end = nullptr;
    const long long parsed = std::strtoll(text, &end, 10);
    if (end == text || *end != '\0')
        return fallback;
    return static_cast<std::int64_t>(parsed);
}

double SettingsSection::get_double(const std::string& key, double fallback) const
{
    const auto found = values_.find(key);
    if (found == values_.end())
        return fallback;

    const char* text = found->second.c_str();
    char* end = nullptr;
    const double parsed = std::strtod(text, &end);
    if (end == text || *end != '\0')
        return fallback;
    return parsed;
}

bool SettingsSection::get_bool(const std::string& key, bool fallback) const
{
    const auto found = values_.find(key);
    if (found == values_.end())
        return fallback;
    if (found->second == "true" || found->second == "1")
        return true;
    if (found->second == "false" || found->second == "0")
        return false;
    return fallback;
}

SettingsSection::Reading SettingsSection::read_int(const std::string& key,
                                                  std::int64_t& out) const
{
    const auto found = values_.find(key);
    if (found == values_.end())
        return Reading::Absent;

    // strtoll rather than stoll: a malformed value must not throw out of a
    // settings read.
    const char* text = found->second.c_str();
    char* end = nullptr;
    const long long parsed = std::strtoll(text, &end, 10);

    // `end != text` alone is not enough: it accepts "12x", stopping at the
    // 'x' and reporting 12. A setting is the whole value or none of it.
    if (end == text || *end != '\0')
        return Reading::Unusable;

    out = static_cast<std::int64_t>(parsed);
    return Reading::Read;
}

SettingsSection::Reading SettingsSection::read_double(const std::string& key,
                                                      double& out) const
{
    const auto found = values_.find(key);
    if (found == values_.end())
        return Reading::Absent;

    const char* text = found->second.c_str();
    char* end = nullptr;
    const double parsed = std::strtod(text, &end);
    if (end == text || *end != '\0')
        return Reading::Unusable;

    out = parsed;
    return Reading::Read;
}

SettingsSection::Reading SettingsSection::read_bool(const std::string& key, bool& out) const
{
    const auto found = values_.find(key);
    if (found == values_.end())
        return Reading::Absent;
    if (found->second == "true" || found->second == "1") {
        out = true;
        return Reading::Read;
    }
    if (found->second == "false" || found->second == "0") {
        out = false;
        return Reading::Read;
    }
    return Reading::Unusable;
}

bool SettingsSection::has(const std::string& key) const
{
    return values_.find(key) != values_.end();
}

bool SettingsSection::remove(const std::string& key)
{
    return values_.erase(key) > 0;
}

std::vector<std::size_t> SettingsSection::indices(const std::string& prefix) const
{
    std::vector<std::size_t> result;
    const std::string search = prefix + ".";

    for (const auto& entry : values_) {
        if (entry.first.compare(0, search.size(), search) != 0)
            continue;

        const std::size_t remainder_start = search.size();
        const std::size_t dot = entry.first.find('.', remainder_start);
        const std::string number = entry.first.substr(
            remainder_start, dot == std::string::npos ? std::string::npos : dot - remainder_start);

        if (number.empty() || number.find_first_not_of("0123456789") != std::string::npos)
            continue;

        const std::size_t index = static_cast<std::size_t>(std::strtoull(number.c_str(), nullptr, 10));
        if (std::find(result.begin(), result.end(), index) == result.end())
            result.push_back(index);
    }

    std::sort(result.begin(), result.end());
    return result;
}

SettingsSection& SettingsStore::section(const std::string& name)
{
    const auto existing = sections_.find(name);
    if (existing != sections_.end())
        return existing->second;

    order_.push_back(name);
    return sections_[name];
}

const SettingsSection* SettingsStore::find_section(const std::string& name) const noexcept
{
    const auto found = sections_.find(name);
    return found == sections_.end() ? nullptr : &found->second;
}

bool SettingsStore::has_section(const std::string& name) const noexcept
{
    return sections_.find(name) != sections_.end();
}

bool SettingsStore::remove_section(const std::string& name)
{
    if (sections_.erase(name) == 0)
        return false;
    order_.erase(std::remove(order_.begin(), order_.end(), name), order_.end());
    return true;
}

std::vector<std::string> SettingsStore::section_names() const
{
    return order_;
}

std::string SettingsStore::to_text() const
{
    std::ostringstream out;

    out << "# VC Composer settings\n";
    out << "# Hand-editable. Keys are ordered within a section; sections are\n";
    out << "# written in the order they were created.\n";
    out << "version = " << version_ << "\n";

    for (const std::string& name : order_) {
        const auto found = sections_.find(name);
        if (found == sections_.end())
            continue;

        out << "\n[" << name << "]\n";
        // std::map iterates in key order, so a written file is stable
        // regardless of the order values were set.
        for (const auto& entry : found->second.values())
            out << entry.first << " = " << escape_value(entry.second) << "\n";
    }

    return out.str();
}

bool SettingsStore::from_text(const std::string& text, std::size_t* skipped_lines)
{
    clear();

    std::size_t skipped = 0;
    bool found_version = false;
    std::string current_section;

    std::istringstream in(text);
    std::string line;

    while (std::getline(in, line)) {
        const std::string trimmed = trim(line);

        if (trimmed.empty() || trimmed[0] == '#')
            continue;

        // Section header.
        if (trimmed.front() == '[') {
            if (trimmed.back() != ']' || trimmed.size() < 3) {
                ++skipped;
                continue;
            }
            current_section = trimmed.substr(1, trimmed.size() - 2);
            section(current_section);  // Create it even if it ends up empty.
            continue;
        }

        const std::size_t equals = trimmed.find('=');
        if (equals == std::string::npos) {
            ++skipped;
            continue;
        }

        const std::string key = trim(trimmed.substr(0, equals));
        const std::string value = trim(trimmed.substr(equals + 1));

        if (key.empty()) {
            ++skipped;
            continue;
        }

        if (current_section.empty()) {
            if (key == "version") {
                const char* digits = value.c_str();
                char* end = nullptr;
                const long parsed = std::strtol(digits, &end, 10);
                if (end != digits && *end == '\0') {
                    version_ = static_cast<int>(parsed);
                    found_version = true;
                } else {
                    ++skipped;
                }
            } else {
                // A key outside any section, other than the version header.
                ++skipped;
            }
            continue;
        }

        section(current_section).set(key, unescape_value(value));
    }

    if (skipped_lines != nullptr)
        *skipped_lines = skipped;

    // A document with no version header is not a settings file - it could
    // be anything, and guessing at its layout risks misreading the user's
    // data.
    return found_version;
}

void SettingsStore::clear()
{
    version_ = kSettingsFormatVersion;
    sections_.clear();
    order_.clear();
}

}  // namespace vcc::composer
