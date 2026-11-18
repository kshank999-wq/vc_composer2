#include "vcc/project/Mackie.h"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace vcc::project::mackie {

namespace {
constexpr std::uint8_t kSysexHeader[] = {0xF0, 0x00, 0x00, 0x66, 0x14};

int clamp_int(int v, int lo, int hi) { return std::max(lo, std::min(hi, v)); }
double clamp01(double v) { return std::max(0.0, std::min(1.0, v)); }

/// Relative encoders: bit 6 is the direction (set: anticlockwise), the low six bits the steps.
int relative(std::uint8_t v)
{
    const int steps = v & 0x3F;
    return (v & 0x40) ? -steps : steps;
}
}  // namespace

std::optional<Event> decode(std::uint8_t status, std::uint8_t data1, std::uint8_t data2)
{
    const int type = status & 0xF0, channel = status & 0x0F;
    Event e;
    if (type == 0xE0) {
        // Faders: 14-bit pitch bend, channels 1-8 the strips, 9 the master.
        if (channel > kMasterStrip) return std::nullopt;
        e.kind = Event::Kind::Fader;
        e.strip = channel;
        e.value = static_cast<double>((data2 << 7) | data1) / 16383.0;
        return e;
    }
    if ((type == 0x90 || type == 0x80) && channel == 0) {
        const bool pressed = type == 0x90 && data2 > 0;
        if (data1 >= FaderTouch1 && data1 <= FaderTouchMaster) {
            e.kind = Event::Kind::FaderTouch;
            e.strip = data1 == FaderTouchMaster ? kMasterStrip : data1 - FaderTouch1;
            e.pressed = pressed;
            return e;
        }
        if (data1 > Scrub) return std::nullopt;
        e.kind = Event::Kind::Button;
        e.button = data1;
        e.pressed = pressed;
        return e;
    }
    if (type == 0xB0 && channel == 0) {
        if (data1 >= 0x10 && data1 <= 0x17) {
            e.kind = Event::Kind::VPot;
            e.strip = data1 - 0x10;
            e.delta = relative(data2);
            return e;
        }
        if (data1 == 0x3C) {
            e.kind = Event::Kind::Jog;
            e.delta = relative(data2);
            return e;
        }
    }
    return std::nullopt;
}

Bytes fader(int strip, double value)
{
    const int v = static_cast<int>(std::lround(clamp01(value) * 16383.0));
    return {static_cast<std::uint8_t>(0xE0 | clamp_int(strip, 0, kMasterStrip)), static_cast<std::uint8_t>(v & 0x7F),
            static_cast<std::uint8_t>((v >> 7) & 0x7F)};
}

Bytes led(int button, Led state)
{
    const std::uint8_t v = state == Led::On ? 0x7F : state == Led::Flash ? 0x01 : 0x00;
    return {0x90, static_cast<std::uint8_t>(clamp_int(button, 0, 0x7F)), v};
}

Bytes vpot_pan(int strip, double pan)
{
    // Mode 0 (a single dot), positions 1..11; 6 is the centre.
    const int pos = 1 + static_cast<int>(std::lround((std::max(-1.0, std::min(1.0, pan)) + 1.0) * 5.0));
    return {0xB0, static_cast<std::uint8_t>(0x30 + clamp_int(strip, 0, 7)), static_cast<std::uint8_t>(pos & 0x0F)};
}

Bytes meter(int strip, double level)
{
    const int lv = clamp_int(static_cast<int>(std::lround(clamp01(level) * 12.0)), 0, 12);
    return {0xD0, static_cast<std::uint8_t>((clamp_int(strip, 0, 7) << 4) | lv)};
}

static Bytes lcd_at(int offset, const std::string& text)
{
    Bytes b(std::begin(kSysexHeader), std::end(kSysexHeader));
    b.push_back(0x12);
    b.push_back(static_cast<std::uint8_t>(clamp_int(offset, 0, 111)));
    for (const char c : text) {
        const auto u = static_cast<unsigned char>(c);
        b.push_back(u >= 0x20 && u < 0x7F ? u : '?');
    }
    b.push_back(0xF7);
    return b;
}

Bytes lcd(int strip, int line, const std::string& text)
{
    std::string t = text.substr(0, 7);
    t.resize(7, ' ');
    return lcd_at(clamp_int(line, 0, 1) * 56 + clamp_int(strip, 0, 7) * 7, t);
}

Bytes lcd_line(int line, const std::string& text56)
{
    std::string t = text56.substr(0, 56);
    t.resize(56, ' ');
    return lcd_at(clamp_int(line, 0, 1) * 56, t);
}

double fader_to_db(double value)
{
    // Piecewise: the top quarter is 0..+6 dB; below, 40 dB per quarter of travel to -inf.
    const double v = clamp01(value);
    if (v <= 0.0) return -144.0;
    if (v >= 0.75) return (v - 0.75) / 0.25 * 6.0;
    // 0.75 -> 0 dB, 0.5 -> -12, 0.25 -> -30, 0.0 -> -inf (smoothly).
    const double x = v / 0.75;                 // 0..1
    return 20.0 * std::log10(std::pow(x, 3.2));  // 1 -> 0 dB; 0.667 -> -11.3; 0.333 -> -30.5
}

double db_to_fader(double db)
{
    if (db >= 0.0) return std::min(1.0, 0.75 + std::min(db, 6.0) / 6.0 * 0.25);
    if (db <= -144.0) return 0.0;
    const double x = std::pow(std::pow(10.0, db / 20.0), 1.0 / 3.2);
    return clamp01(x * 0.75);
}

std::string short_name(const std::string& name, std::size_t width)
{
    if (name.size() <= width) return name;
    // Words, without separators; vowels dropped from all but each word's first letter until it fits.
    std::vector<std::string> words;
    std::string w;
    for (const char c : name) {
        if (std::isalnum(static_cast<unsigned char>(c))) w += c;
        else if (!w.empty()) {
            words.push_back(w);
            w.clear();
        }
    }
    if (!w.empty()) words.push_back(w);
    auto join = [&] {
        std::string s;
        for (const auto& x : words) s += x;
        return s;
    };
    if (join().size() <= width) return join();
    for (auto& x : words) {
        std::string kept;
        for (std::size_t i = 0; i < x.size(); ++i) {
            const char c = static_cast<char>(std::tolower(static_cast<unsigned char>(x[i])));
            if (i > 0 && (c == 'a' || c == 'e' || c == 'i' || c == 'o' || c == 'u')) continue;
            kept += x[i];
        }
        x = kept;
    }
    std::string s = join();
    if (s.size() <= width) return s;
    // Still too long: share the width between the words, first ones first.
    std::string out;
    const std::size_t each = std::max<std::size_t>(1, width / std::max<std::size_t>(1, words.size()));
    for (std::size_t i = 0; i < words.size() && out.size() < width; ++i) {
        const std::size_t room = i + 1 == words.size() ? width - out.size() : std::min(each, width - out.size());
        out += words[i].substr(0, room);
    }
    return out.substr(0, width);
}

}  // namespace vcc::project::mackie
