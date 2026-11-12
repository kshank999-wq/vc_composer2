#include "vcc/notation/Markings.h"

#include <cstddef>
#include <cmath>
#include <cstring>

namespace vcc::notation {

const char* to_string(DynamicLevel level) noexcept
{
    switch (level) {
        case DynamicLevel::Pianississimo: return "ppp";
        case DynamicLevel::Pianissimo:    return "pp";
        case DynamicLevel::Piano:         return "p";
        case DynamicLevel::MezzoPiano:    return "mp";
        case DynamicLevel::MezzoForte:    return "mf";
        case DynamicLevel::Forte:         return "f";
        case DynamicLevel::Fortissimo:    return "ff";
        case DynamicLevel::Fortississimo: break;
    }
    return "fff";
}

bool dynamic_from_string(const std::string& text, DynamicLevel& out) noexcept
{
    struct Entry { const char* name; DynamicLevel level; };
    static const Entry entries[] = {
        {"ppp", DynamicLevel::Pianississimo}, {"pp", DynamicLevel::Pianissimo},
        {"p", DynamicLevel::Piano},           {"mp", DynamicLevel::MezzoPiano},
        {"mf", DynamicLevel::MezzoForte},     {"f", DynamicLevel::Forte},
        {"ff", DynamicLevel::Fortissimo},     {"fff", DynamicLevel::Fortississimo},
    };

    for (const Entry& entry : entries) {
        if (text == entry.name) {
            out = entry.level;
            return true;
        }
    }
    return false;
}

bool DynamicMarking::is_valid() const noexcept
{
    if (tick < 0)
        return false;
    return static_cast<std::size_t>(level) < kDynamicLevelCount;
}

ArticulationMask to_mask(Articulation articulation) noexcept
{
    return static_cast<ArticulationMask>(articulation);
}

ArticulationMask operator|(Articulation a, Articulation b) noexcept
{
    return static_cast<ArticulationMask>(a) | static_cast<ArticulationMask>(b);
}

bool has_articulation(ArticulationMask mask, Articulation articulation) noexcept
{
    const auto bit = static_cast<ArticulationMask>(articulation);
    return bit != 0 && (mask & bit) == bit;
}

const char* to_string(Articulation articulation) noexcept
{
    switch (articulation) {
        case Articulation::Accent:        return "accent";
        case Articulation::Staccato:      return "staccato";
        case Articulation::Staccatissimo: return "staccatissimo";
        case Articulation::Tenuto:        return "tenuto";
        case Articulation::Marcato:       return "marcato";
        case Articulation::Legato:        return "legato";
        case Articulation::Portato:       return "portato";
        case Articulation::Spiccato:      return "spiccato";
        case Articulation::Pizzicato:     return "pizzicato";
        case Articulation::Tremolo:       return "tremolo";
        case Articulation::Trill:         return "trill";
        case Articulation::Mordent:       return "mordent";
        case Articulation::Turn:          return "turn";
        case Articulation::Fermata:       return "fermata";
        case Articulation::UpBow:         return "up bow";
        case Articulation::DownBow:       return "down bow";
        case Articulation::Harmonic:      return "harmonic";
        case Articulation::Mute:          return "mute";
        case Articulation::Falloff:       return "falloff";
        case Articulation::Doit:          return "doit";
        case Articulation::Plop:          return "plop";
        case Articulation::Scoop:         return "scoop";
        case Articulation::None:          break;
    }
    return "none";
}

const std::vector<Articulation>& all_articulations()
{
    static const std::vector<Articulation> kAll = {
        Articulation::Accent,    Articulation::Staccato,  Articulation::Staccatissimo,
        Articulation::Tenuto,    Articulation::Marcato,   Articulation::Legato,
        Articulation::Portato,   Articulation::Spiccato,  Articulation::Pizzicato,
        Articulation::Tremolo,   Articulation::Trill,     Articulation::Mordent,
        Articulation::Turn,      Articulation::Fermata,   Articulation::UpBow,
        Articulation::DownBow,   Articulation::Harmonic,  Articulation::Mute,
        Articulation::Falloff,   Articulation::Doit,      Articulation::Plop,
        Articulation::Scoop,
    };
    return kAll;
}

bool articulation_from_string(const std::string& text, Articulation& out) noexcept
{
    if (text == to_string(Articulation::None)) {
        out = Articulation::None;
        return true;
    }
    for (const Articulation articulation : all_articulations()) {
        if (text == to_string(articulation)) {
            out = articulation;
            return true;
        }
    }
    return false;
}

void format_articulations(ArticulationMask mask, char* out, std::size_t capacity) noexcept
{
    if (out == nullptr || capacity == 0)
        return;
    out[0] = '\0';

    std::size_t used = 0;
    bool first = true;
    for (const Articulation articulation : all_articulations()) {
        if (!has_articulation(mask, articulation))
            continue;
        const char* name = to_string(articulation);
        const char* separator = first ? "" : ",";
        const std::size_t needed = std::strlen(separator) + std::strlen(name);
        if (used + needed + 1 > capacity)
            break;
        std::memcpy(out + used, separator, std::strlen(separator));
        used += std::strlen(separator);
        std::memcpy(out + used, name, std::strlen(name));
        used += std::strlen(name);
        out[used] = '\0';
        first = false;
    }

    if (first) {
        const char* none = "none";
        const std::size_t length = std::strlen(none) + 1 <= capacity ? std::strlen(none)
                                                                     : capacity - 1;
        std::memcpy(out, none, length);
        out[length] = '\0';
    }
}

const char* to_string(HairpinKind kind) noexcept
{
    return kind == HairpinKind::Crescendo ? "crescendo" : "diminuendo";
}

const char* to_string(CurveShape shape) noexcept
{
    switch (shape) {
        case CurveShape::Linear:      return "linear";
        case CurveShape::Exponential: return "exponential";
        case CurveShape::Logarithmic: return "logarithmic";
        case CurveShape::SCurve:      break;
    }
    return "s-curve";
}

bool hairpin_kind_from_string(const std::string& text, HairpinKind& out) noexcept
{
    for (const HairpinKind kind : {HairpinKind::Crescendo, HairpinKind::Diminuendo}) {
        if (text == to_string(kind)) {
            out = kind;
            return true;
        }
    }
    return false;
}

bool curve_shape_from_string(const std::string& text, CurveShape& out) noexcept
{
    for (int index = 0; index <= static_cast<int>(CurveShape::SCurve); ++index) {
        const auto shape = static_cast<CurveShape>(index);
        if (text == to_string(shape)) {
            out = shape;
            return true;
        }
    }
    return false;
}

double evaluate_curve(CurveShape shape, double position) noexcept
{
    // Clamp rather than extrapolate: a hairpin has endpoints, and a caller
    // asking outside them wants the endpoint value.
    if (!(position > 0.0))
        return 0.0;
    if (position >= 1.0)
        return 1.0;

    switch (shape) {
        case CurveShape::Linear:
            return position;
        case CurveShape::Exponential:
            // Slow start, fast finish. Squaring is the mild version; a
            // steeper exponent makes long crescendos feel like nothing is
            // happening until the end.
            return position * position;
        case CurveShape::Logarithmic:
            return std::sqrt(position);
        case CurveShape::SCurve:
            // Smoothstep: eased at both ends, no discontinuity in slope.
            return position * position * (3.0 - 2.0 * position);
    }
    return position;
}

bool Hairpin::is_valid() const noexcept
{
    if (start_tick < 0 || end_tick <= start_tick)
        return false;
    if (has_start_level && static_cast<std::size_t>(start_level) >= kDynamicLevelCount)
        return false;
    if (has_end_level && static_cast<std::size_t>(end_level) >= kDynamicLevelCount)
        return false;

    // A crescendo that ends quieter than it starts is a contradiction, and
    // silently rendering it backwards would be worse than refusing it.
    if (has_start_level && has_end_level) {
        if (kind == HairpinKind::Crescendo && end_level < start_level)
            return false;
        if (kind == HairpinKind::Diminuendo && end_level > start_level)
            return false;
    }
    return true;
}

double TempoMarking::quarter_notes_per_minute() const noexcept
{
    // "Dotted quarter = 60" in 6/8 means 60 dotted quarters per minute,
    // which is 90 quarter notes per minute. Converting here keeps the
    // written marking intact while giving the timeline what it needs.
    const Duration unit{beat_unit, beat_unit_dots, {}};
    const Ticks unit_ticks = unit.ticks();
    if (unit_ticks <= 0)
        return beats_per_minute;
    return beats_per_minute * static_cast<double>(unit_ticks)
         / static_cast<double>(kTicksPerQuarter);
}

bool TempoMarking::is_valid() const noexcept
{
    if (tick < 0)
        return false;
    if (!(beats_per_minute > 0.0) || beats_per_minute > 1000.0)
        return false;
    if (beat_unit_dots > 3)
        return false;
    return Duration{beat_unit, beat_unit_dots, {}}.is_valid();
}

}  // namespace vcc::notation
