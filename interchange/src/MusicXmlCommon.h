// Shared by MusicXML import and export: names, intervals, numbers.
#pragma once

#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>

#include "vcc/notation/Markings.h"
#include "vcc/notation/MusicalTime.h"
#include "vcc/notation/Pitch.h"

namespace vcc::interchange::detail {

inline const char* type_name(notation::NoteValue v) noexcept
{
    switch (v) {
        case notation::NoteValue::Breve: return "breve";
        case notation::NoteValue::Whole: return "whole";
        case notation::NoteValue::Half: return "half";
        case notation::NoteValue::Quarter: return "quarter";
        case notation::NoteValue::Eighth: return "eighth";
        case notation::NoteValue::Sixteenth: return "16th";
        case notation::NoteValue::ThirtySecond: return "32nd";
        case notation::NoteValue::SixtyFourth: return "64th";
    }
    return "quarter";
}

inline std::optional<notation::NoteValue> type_from_name(std::string_view s) noexcept
{
    using notation::NoteValue;
    if (s == "breve") return NoteValue::Breve;
    if (s == "whole") return NoteValue::Whole;
    if (s == "half") return NoteValue::Half;
    if (s == "quarter") return NoteValue::Quarter;
    if (s == "eighth") return NoteValue::Eighth;
    if (s == "16th") return NoteValue::Sixteenth;
    if (s == "32nd") return NoteValue::ThirtySecond;
    if (s == "64th") return NoteValue::SixtyFourth;
    return std::nullopt;
}

inline int floor_div(int a, int b) noexcept { return a >= 0 ? a / b : -((-a + b - 1) / b); }
inline int floor_mod(int a, int b) noexcept { return a - floor_div(a, b) * b; }

/// Diatonic steps of the usual spelling of an interval of `semitones`
/// (positive or negative): a major second for 2, a perfect fifth for 7, an
/// augmented fourth for the tritone.
inline int diatonic_for_semitones(int semitones) noexcept
{
    static const int kSteps[12] = {0, 1, 1, 2, 2, 3, 3, 4, 5, 5, 6, 6};
    const int sign = semitones < 0 ? -1 : 1;
    const int a = semitones < 0 ? -semitones : semitones;
    return sign * ((a / 12) * 7 + kSteps[a % 12]);
}

/// Change in key-signature fifths when transposing by an interval of
/// `diatonic` steps and `chromatic` semitones.
inline int fifths_delta(int diatonic, int chromatic) noexcept { return 7 * chromatic - 12 * diatonic; }

/// Brings a key into -7..7 by enharmonic respelling.
inline int normalise_fifths(int fifths) noexcept
{
    while (fifths > 7) fifths -= 12;
    while (fifths < -7) fifths += 12;
    return fifths;
}

/// Transposes a written pitch by an interval, keeping the letter
/// arithmetic, and respells only when the result would need more than a
/// double accidental.
inline notation::Pitch transpose_pitch(const notation::Pitch& p, int diatonic, int chromatic,
                                       const notation::KeySignature& key) noexcept
{
    const int midi = (p.octave + 1) * 12 + notation::semitones_above_c(p.step) + p.alteration;
    const int index = notation::diatonic_index(p) + diatonic;
    notation::Pitch out;
    out.octave = static_cast<std::int8_t>(floor_div(index, 7));
    out.step = static_cast<notation::Step>(floor_mod(index, 7));
    out.alteration = 0;
    const int natural = (out.octave + 1) * 12 + notation::semitones_above_c(out.step);
    const int target = midi + chromatic;
    const int alter = target - natural;
    if (alter >= -2 && alter <= 2) {
        out.alteration = static_cast<std::int8_t>(alter);
        return out;
    }
    return notation::spell_midi_note(target, key);
}

// --- Numbers ------------------------------------------------------------------

inline std::string_view trim_view(std::string_view s) noexcept
{
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\n' || s.front() == '\r'))
        s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\n' || s.back() == '\r'))
        s.remove_suffix(1);
    return s;
}

/// Locale-independent decimal parse; nullopt for anything that is not a
/// complete number.
inline std::optional<double> parse_double(std::string_view s) noexcept
{
    s = trim_view(s);
    if (!s.empty() && s.front() == '+') s.remove_prefix(1);
    if (s.empty()) return std::nullopt;
    double v = 0;
    auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
    if (ec != std::errc() || ptr != s.data() + s.size()) return std::nullopt;
    return v;
}

inline std::optional<long long> parse_int(std::string_view s) noexcept
{
    s = trim_view(s);
    if (!s.empty() && s.front() == '+') s.remove_prefix(1);
    if (s.empty()) return std::nullopt;
    long long v = 0;
    auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
    if (ec != std::errc() || ptr != s.data() + s.size()) {
        // "4.0" and similar: accept an integral decimal.
        auto d = parse_double(s);
        if (d && *d == static_cast<double>(static_cast<long long>(*d)) && *d > -1e15 && *d < 1e15)
            return static_cast<long long>(*d);
        return std::nullopt;
    }
    return v;
}

/// Up to two decimals, trailing zeros dropped, never an exponent: "120",
/// "92.5", "63.33".
inline std::string format_number(double v)
{
    if (!(v > -1e12 && v < 1e12)) v = 0;
    const bool negative = v < 0;
    const auto hundredths = static_cast<long long>((negative ? -v : v) * 100.0 + 0.5);
    std::string out = (negative && hundredths != 0 ? "-" : "") + std::to_string(hundredths / 100);
    const long long frac = hundredths % 100;
    if (frac != 0) {
        out += '.';
        out += static_cast<char>('0' + frac / 10);
        if (frac % 10 != 0) out += static_cast<char>('0' + frac % 10);
    }
    return out;
}

inline long long gcd_ll(long long a, long long b) noexcept
{
    if (a < 0) a = -a;
    if (b < 0) b = -b;
    while (b != 0) {
        const long long t = a % b;
        a = b;
        b = t;
    }
    return a;
}

}  // namespace vcc::interchange::detail
