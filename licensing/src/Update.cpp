#include "vcc/licensing/Update.h"

#include <cctype>
#include <tuple>

namespace vcc::licensing {

namespace {

/// Reads a whole number at `at`, moving past it; -1 if there is none.
int number(const std::string& s, std::size_t& at)
{
    if (at >= s.size() || !std::isdigit(static_cast<unsigned char>(s[at]))) return -1;
    long n = 0;
    while (at < s.size() && std::isdigit(static_cast<unsigned char>(s[at]))) {
        n = n * 10 + (s[at++] - '0');
        if (n > 1000000) return -1;
    }
    return static_cast<int>(n);
}

}  // namespace

BuildVersion parse_version(const std::string& text)
{
    BuildVersion v;
    std::size_t at = (!text.empty() && (text[0] == 'v' || text[0] == 'V')) ? 1 : 0;
    int parts[3];
    for (int i = 0; i < 3; ++i) {
        parts[i] = number(text, at);
        if (parts[i] < 0) return v;
        if (i < 2) {
            if (at >= text.size() || text[at] != '.') return v;
            ++at;
        }
    }
    v.major = parts[0];
    v.minor = parts[1];
    v.patch = parts[2];
    if (at == text.size()) {
        v.ok = true;
        return v;
    }
    const std::string tag = "-preview.";
    if (text.compare(at, tag.size(), tag) != 0) return v;
    at += tag.size();
    v.preview = number(text, at);
    v.ok = v.preview >= 0 && at == text.size();
    return v;
}

bool is_newer(const BuildVersion& a, const BuildVersion& b)
{
    if (!a.ok || !b.ok) return false;
    const auto core_a = std::tie(a.major, a.minor, a.patch), core_b = std::tie(b.major, b.minor, b.patch);
    if (core_a != core_b) return core_a > core_b;
    if (a.preview == b.preview) return false;
    if (a.preview < 0) return true;   // a release beats its previews
    if (b.preview < 0) return false;
    return a.preview > b.preview;
}

bool is_newer(const std::string& a, const std::string& b) { return is_newer(parse_version(a), parse_version(b)); }

std::optional<UpdateOffer> pick_update(const json::Value& releases, const std::string& platform, const std::string& current)
{
    const auto* builds = releases.get("builds");
    if (!builds || !builds->is_array()) return std::nullopt;
    std::optional<UpdateOffer> best;
    for (const auto& b : builds->as_array()) {
        if (b.get_string("platform") != platform) continue;
        const auto version = b.get_string("version");
        if (!is_newer(version, current)) continue;
        if (best && !is_newer(version, best->version)) continue;
        best = UpdateOffer{version, b.get_string("releaseNotes"), b.get_string("sha256"), b.get_string("minimumOsVersion")};
    }
    return best;
}

}  // namespace vcc::licensing
