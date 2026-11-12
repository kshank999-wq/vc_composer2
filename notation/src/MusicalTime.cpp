#include "vcc/notation/MusicalTime.h"

#include <cstddef>

namespace vcc::notation {

const char* to_string(NoteValue value) noexcept
{
    switch (value) {
        case NoteValue::Breve:        return "breve";
        case NoteValue::Whole:        return "whole";
        case NoteValue::Half:         return "half";
        case NoteValue::Quarter:      return "quarter";
        case NoteValue::Eighth:       return "eighth";
        case NoteValue::Sixteenth:    return "16th";
        case NoteValue::ThirtySecond: return "32nd";
        case NoteValue::SixtyFourth:  break;
    }
    return "64th";
}

bool note_value_from_string(const std::string& text, NoteValue& out) noexcept
{
    for (int index = 0; index <= static_cast<int>(NoteValue::SixtyFourth); ++index) {
        const auto value = static_cast<NoteValue>(index);
        if (text == to_string(value)) {
            out = value;
            return true;
        }
    }
    return false;
}

Ticks base_ticks(NoteValue value) noexcept
{
    // A breve is two whole notes; each successive value halves.
    switch (value) {
        case NoteValue::Breve:        return kTicksPerWhole * 2;
        case NoteValue::Whole:        return kTicksPerWhole;
        case NoteValue::Half:         return kTicksPerWhole / 2;
        case NoteValue::Quarter:      return kTicksPerWhole / 4;
        case NoteValue::Eighth:       return kTicksPerWhole / 8;
        case NoteValue::Sixteenth:    return kTicksPerWhole / 16;
        case NoteValue::ThirtySecond: return kTicksPerWhole / 32;
        case NoteValue::SixtyFourth:  break;
    }
    return kTicksPerWhole / 64;
}

bool Tuplet::is_valid() const noexcept
{
    if (actual == 0 || normal == 0)
        return false;
    // Beyond about 32 the ratio stops being notatable and starts being a
    // way to smuggle arbitrary rationals into the model.
    return actual <= 32 && normal <= 32;
}

bool operator==(const Tuplet& a, const Tuplet& b) noexcept
{
    return a.actual == b.actual && a.normal == b.normal;
}

bool operator!=(const Tuplet& a, const Tuplet& b) noexcept
{
    return !(a == b);
}

Ticks Duration::ticks() const noexcept
{
    if (!is_valid())
        return 0;

    const Ticks base = base_ticks(value);

    // Dots: each adds half the previous addition. One dot is 3/2 of the
    // base, two dots 7/4, three dots 15/8. Computed as
    // base * (2^(dots+1) - 1) / 2^dots, exact for every combination
    // is_valid admits.
    const Ticks numerator = (Ticks{1} << (dots + 1)) - 1;
    const Ticks denominator = Ticks{1} << dots;
    Ticks result = base * numerator / denominator;

    // Tuplet: `actual` in the time of `normal` - and inside another, that
    // one's ratio as well, in one division so the product stays exact.
    if (tuplet.is_tuplet()) {
        result = result * tuplet.normal * outer.normal / (tuplet.actual * outer.actual);
    }

    return result;
}

bool Duration::is_valid() const noexcept
{
    if (dots > 3)
        return false;
    if (!tuplet.is_valid() || !outer.is_valid())
        return false;
    // An outer ratio needs a tuplet inside it.
    if (outer.is_tuplet() && !tuplet.is_tuplet())
        return false;

    // A value this short with this many dots would not divide exactly, and
    // an inexact duration is worse than a rejected one.
    const Ticks base = base_ticks(value);
    const Ticks denominator = Ticks{1} << dots;
    if (base % denominator != 0)
        return false;

    if (tuplet.is_tuplet()) {
        const Ticks dotted = base * ((Ticks{1} << (dots + 1)) - 1) / denominator;
        if (dotted * tuplet.normal * outer.normal % (tuplet.actual * outer.actual) != 0)
            return false;
    }
    return true;
}

bool operator==(const Duration& a, const Duration& b) noexcept
{
    return a.value == b.value && a.dots == b.dots && a.tuplet == b.tuplet && a.outer == b.outer;
}

bool operator!=(const Duration& a, const Duration& b) noexcept
{
    return !(a == b);
}

Duration whole() noexcept { return Duration{NoteValue::Whole, 0, {}}; }

Duration half(std::uint8_t dots) noexcept { return Duration{NoteValue::Half, dots, {}}; }

Duration quarter(std::uint8_t dots) noexcept { return Duration{NoteValue::Quarter, dots, {}}; }

Duration eighth(std::uint8_t dots) noexcept { return Duration{NoteValue::Eighth, dots, {}}; }

Duration sixteenth(std::uint8_t dots) noexcept { return Duration{NoteValue::Sixteenth, dots, {}}; }

Duration triplet(NoteValue value) noexcept { return Duration{value, 0, Tuplet{3, 2}}; }

std::size_t decompose_ticks(Ticks ticks, Duration* out, std::size_t capacity) noexcept
{
    if (out == nullptr || capacity == 0 || ticks <= 0)
        return 0;

    // Greedy longest-first over plain and dotted values. This is how a tie
    // chain is conventionally notated, and because every candidate length
    // divides kTicksPerQuarter exactly there is no rounding to accumulate.
    static const NoteValue kValues[] = {
        NoteValue::Breve, NoteValue::Whole, NoteValue::Half, NoteValue::Quarter,
        NoteValue::Eighth, NoteValue::Sixteenth, NoteValue::ThirtySecond,
        NoteValue::SixtyFourth,
    };

    std::size_t written = 0;
    Ticks remaining = ticks;

    while (remaining > 0) {
        if (written >= capacity)
            return 0;  // Cannot express it in the space available.

        bool matched = false;
        for (const NoteValue value : kValues) {
            // Try the most dots first at each value, so a dotted half is
            // preferred over half-tied-to-quarter.
            for (int dots = 3; dots >= 0; --dots) {
                const Duration candidate{value, static_cast<std::uint8_t>(dots), {}};
                if (!candidate.is_valid())
                    continue;
                const Ticks length = candidate.ticks();
                if (length > 0 && length <= remaining) {
                    out[written++] = candidate;
                    remaining -= length;
                    matched = true;
                    break;
                }
            }
            if (matched)
                break;
        }

        if (!matched)
            return 0;  // Shorter than a 64th, or otherwise not notatable.
    }

    return written;
}

}  // namespace vcc::notation
