#include "vcc/licensing/Standing.h"

#include <cstdio>

namespace vcc::licensing {

namespace {

constexpr std::int64_t kDay = 86400;

std::int64_t days_from_civil(int y, unsigned m, unsigned d)
{
    y -= m <= 2;
    const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
    const auto yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

int digits(const std::string& s, std::size_t at, std::size_t n)
{
    if (at + n > s.size()) return -1;
    int v = 0;
    for (std::size_t i = at; i < at + n; ++i) {
        if (s[i] < '0' || s[i] > '9') return -1;
        v = v * 10 + (s[i] - '0');
    }
    return v;
}

std::string lapse_words(const DeskStanding& s)
{
    if (s.status == "expired" || s.status == "active") {
        const std::string on = s.expires_at.empty() ? std::string() : " on " + s.expires_at.substr(0, 10);
        if (s.grant == "academic") return "Your VC Composer academic licence ended" + on + ".";
        return "Your VC Composer subscription ended" + on + ".";
    }
    return "This copy's licence is no longer active.";
}

std::string day_of(std::int64_t seconds) { return format_iso(seconds).substr(0, 10); }

}  // namespace

std::optional<std::int64_t> parse_iso(const std::string& t)
{
    const int y = digits(t, 0, 4), mo = digits(t, 5, 2), d = digits(t, 8, 2);
    if (y < 1970 || mo < 1 || mo > 12 || d < 1 || d > 31 || t[4] != '-' || t[7] != '-') return std::nullopt;
    std::int64_t s = days_from_civil(y, static_cast<unsigned>(mo), static_cast<unsigned>(d)) * kDay;
    if (t.size() == 10) return s;
    if (t[10] != 'T' && t[10] != ' ') return std::nullopt;
    const int h = digits(t, 11, 2), mi = digits(t, 14, 2), se = digits(t, 17, 2);
    if (h < 0 || h > 23 || mi < 0 || mi > 59 || se < 0 || se > 60) return std::nullopt;
    s += h * 3600 + mi * 60 + se;
    std::size_t i = 19;
    if (i < t.size() && t[i] == '.') {
        ++i;
        while (i < t.size() && t[i] >= '0' && t[i] <= '9') ++i;
    }
    if (i == t.size() || t[i] == 'Z') return i == t.size() || i + 1 == t.size() ? std::optional<std::int64_t>(s) : std::nullopt;
    if (t[i] == '+' || t[i] == '-') {
        const int oh = digits(t, i + 1, 2);
        const int om = t.size() > i + 3 && t[i + 3] == ':' ? digits(t, i + 4, 2) : digits(t, i + 3, 2);
        if (oh < 0 || om < 0) return std::nullopt;
        const std::int64_t off = oh * 3600 + om * 60;
        return t[i] == '+' ? s - off : s + off;
    }
    return std::nullopt;
}

std::string format_iso(std::int64_t s)
{
    std::int64_t days = s >= 0 ? s / kDay : (s - kDay + 1) / kDay;
    std::int64_t rem = s - days * kDay;
    // civil_from_days
    days += 719468;
    const std::int64_t era = (days >= 0 ? days : days - 146096) / 146097;
    const auto doe = static_cast<unsigned>(days - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    const unsigned d = doy - (153 * mp + 2) / 5 + 1;
    const unsigned m = mp < 10 ? mp + 3 : mp - 9;
    const auto y = static_cast<long long>(yoe) + era * 400 + (m <= 2);
    char out[96];
    std::snprintf(out, sizeof out, "%04lld-%02u-%02uT%02d:%02d:%02d.000Z", y, m, d, static_cast<int>(rem / 3600),
                  static_cast<int>(rem % 3600 / 60), static_cast<int>(rem % 60));
    return out;
}

bool licence_live(const std::string& status, const std::string& expires_at, std::int64_t now)
{
    if (status != "active") return false;
    if (expires_at.empty()) return true;
    const auto end = parse_iso(expires_at);
    return !end || *end > now;  // a date this build cannot read is not a lapse
}

std::optional<Claim> claim_from_response(const json::Value& body)
{
    const auto* s = body.get("standing");
    if (!s || !s->is_object()) return std::nullopt;
    Claim c;
    c.status = s->get_string("status");
    if (c.status != "active" && c.status != "suspended" && c.status != "revoked" && c.status != "expired") return std::nullopt;
    if (const auto* e = s->get("expiresAt"); e && e->is_string()) c.expires_at = e->as_string();
    c.grant = s->get_string("grant");
    return c;
}

json::Value DeskStanding::to_json() const
{
    json::Object o{{"status", status}, {"checkedAt", checked_at}};
    o.set("expiresAt", expires_at.empty() ? json::Value() : json::Value(expires_at));
    o.set("seenLapsedAt", seen_lapsed_at.empty() ? json::Value() : json::Value(seen_lapsed_at));
    if (!grant.empty() && grant != "purchase") o.set("grant", grant);
    return o;
}

std::optional<DeskStanding> DeskStanding::from_json(const json::Value& doc)
{
    if (!doc.is_object() || doc.get_string("status").empty() || doc.get_string("checkedAt").empty()) return std::nullopt;
    DeskStanding s;
    s.status = doc.get_string("status");
    s.checked_at = doc.get_string("checkedAt");
    s.expires_at = doc.get_string("expiresAt");
    s.seen_lapsed_at = doc.get_string("seenLapsedAt");
    s.grant = doc.get_string("grant");
    return s;
}

DeskStanding note_standing(const std::optional<DeskStanding>& previous, const Claim& claim, std::int64_t now)
{
    DeskStanding s;
    s.status = claim.status;
    s.expires_at = claim.expires_at;
    s.checked_at = format_iso(now);
    s.grant = claim.grant == "purchase" ? std::string() : claim.grant;
    if (!licence_live(claim.status, claim.expires_at, now))
        s.seen_lapsed_at = previous && !previous->seen_lapsed_at.empty() ? previous->seen_lapsed_at : s.checked_at;
    return s;
}

SavingStanding saving_standing(const std::optional<DeskStanding>& standing, std::int64_t now, std::optional<bool> activated)
{
    if (activated && !*activated)
        return {false,
                "This computer is not activated yet. Sign in and activate VC Composer to save. Until then your "
                "projects open, play, print and export.",
                false, true};
    if (!standing) return {};
    if (licence_live(standing->status, standing->expires_at, now)) return {};
    const std::string ended = lapse_words(*standing);
    const auto checked = parse_iso(standing->checked_at).value_or(now);
    if (now > checked + kStandingGoodForDays * kDay)
        return {true,
                ended + (standing->grant == "academic"
                             ? " Ask whoever gave it to you to extend it, to install on another computer."
                             : " Renew it to keep saving and to install on another computer."),
                true, false};
    const auto seen = parse_iso(standing->seen_lapsed_at).value_or(checked);
    const std::int64_t bites = seen + kGraceDays * kDay;
    if (now < bites)
        return {true,
                ended + " This copy keeps saving until " + day_of(bites) +
                    ". After that your projects still open, play, print and export, but cannot be saved.",
                true, false};
    return {false,
            ended + " This copy no longer saves. Your projects still open, play, print and export; renew at "
                    "vc-composer.com to save again.",
            false, false};
}

}  // namespace vcc::licensing
