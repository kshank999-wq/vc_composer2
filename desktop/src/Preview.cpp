#include "Preview.h"

#include <ctime>

#ifndef VCC_PREVIEW
#define VCC_PREVIEW 0
#endif
#ifndef VCC_PREVIEW_EXPIRES
#define VCC_PREVIEW_EXPIRES ""
#endif

namespace vcc::desktop::preview {

namespace {

/// Days since 1970-01-01 for a "YYYY-MM-DD" date (civil calendar), or a
/// sentinel when it is not one.
long day_number(const juce::String& date)
{
    const auto parts = juce::StringArray::fromTokens(date.trim(), "-", "");
    if (parts.size() != 3) return -1000000;
    int y = parts[0].getIntValue();
    const int m = parts[1].getIntValue(), d = parts[2].getIntValue();
    if (y < 2000 || m < 1 || m > 12 || d < 1 || d > 31) return -1000000;
    // Howard Hinnant's days_from_civil.
    y -= m <= 2;
    const long era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = static_cast<unsigned>((153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1);
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<long>(doe) - 719468;
}

juce::String today_utc()
{
    const std::time_t now = std::time(nullptr);
    std::tm utc{};
#if JUCE_WINDOWS
    gmtime_s(&utc, &now);
#else
    gmtime_r(&now, &utc);
#endif
    char text[16];
    std::strftime(text, sizeof text, "%Y-%m-%d", &utc);
    return text;
}

}  // namespace

bool is_preview() noexcept { return VCC_PREVIEW != 0; }

juce::String expires() { return is_preview() ? juce::String(VCC_PREVIEW_EXPIRES) : juce::String(); }

int days_left(const juce::String& today)
{
    if (!is_preview()) return 1 << 20;
    const long end = day_number(expires());
    if (end < -100000) return 1 << 20;  // no date: never ends (a local preview build)
    return static_cast<int>(end - day_number(today.isEmpty() ? today_utc() : today));
}

bool ended(const juce::String& today) { return is_preview() && days_left(today) < 0; }

juce::String status(const juce::String& today)
{
    if (!is_preview()) return {};
    if (expires().isEmpty()) return "Preview";
    const int left = days_left(today);
    if (left < 0) return "Preview ended " + expires();
    return "Preview - " + juce::String(left) + (left == 1 ? " day left" : " days left");
}

}  // namespace vcc::desktop::preview
