#include "vcc/theory/Pitch.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>

namespace vcc::theory {

namespace {

int mod(int a, int m) noexcept
{
    int r = a % m;
    return r < 0 ? r + m : r;
}

const char* const kLetters = "CDEFGAB";

/// Reduces a semitone difference to the alteration nearest zero (-6..+6).
int nearest_alteration(int diff) noexcept
{
    int d = mod(diff, 12);
    return d > 6 ? d - 12 : d;
}

std::string lower(std::string s)
{
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

}  // namespace

int letter_pitch_class(int letter) noexcept
{
    static const int pcs[7] = {0, 2, 4, 5, 7, 9, 11};
    return pcs[mod(letter, 7)];
}

// --- SpelledPitch -----------------------------------------------------------

int SpelledPitch::pitch_class() const noexcept { return mod(letter_pitch_class(letter) + alteration, 12); }

std::string SpelledPitch::name() const
{
    std::string s(1, kLetters[mod(letter, 7)]);
    if (alteration == 2) s += "x";
    else if (alteration > 0) s.append(static_cast<std::size_t>(alteration), '#');
    else if (alteration < 0) s.append(static_cast<std::size_t>(-alteration), 'b');
    return s;
}

std::string SpelledPitch::display_name() const
{
    std::string s(1, kLetters[mod(letter, 7)]);
    if (alteration == 2) s += "\xF0\x9D\x84\xAA";        // double sharp
    else if (alteration == -2) s += "\xF0\x9D\x84\xAB";  // double flat
    else if (alteration > 0) for (int i = 0; i < alteration; ++i) s += "\xE2\x99\xAF";
    else if (alteration < 0) for (int i = 0; i < -alteration; ++i) s += "\xE2\x99\xAD";
    return s;
}

std::optional<SpelledPitch> SpelledPitch::parse(const std::string& text)
{
    if (text.empty()) return std::nullopt;
    char l = static_cast<char>(std::toupper(static_cast<unsigned char>(text[0])));
    const char* found = std::strchr(kLetters, l);
    if (!found || l == '\0') return std::nullopt;
    SpelledPitch p{static_cast<int>(found - kLetters), 0};
    std::size_t i = 1;
    while (i < text.size()) {
        if (text[i] == '#') { ++p.alteration; ++i; }
        else if (text[i] == 'x') { p.alteration += 2; ++i; }
        else if (text[i] == 'b') { --p.alteration; ++i; }
        else if (text.compare(i, 3, "\xE2\x99\xAF") == 0) { ++p.alteration; i += 3; }
        else if (text.compare(i, 3, "\xE2\x99\xAD") == 0) { --p.alteration; i += 3; }
        else if (text.compare(i, 3, "\xE2\x99\xAE") == 0) { i += 3; }  // natural sign
        else if (text.compare(i, 4, "\xF0\x9D\x84\xAA") == 0) { p.alteration += 2; i += 4; }
        else if (text.compare(i, 4, "\xF0\x9D\x84\xAB") == 0) { p.alteration -= 2; i += 4; }
        else return std::nullopt;
    }
    if (p.alteration < -2 || p.alteration > 2) return std::nullopt;
    return p;
}

SpelledPitch SpelledPitch::transposed(int steps, int semitones) const noexcept
{
    SpelledPitch out;
    out.letter = mod(letter + steps, 7);
    int target_pc = pitch_class() + semitones;
    out.alteration = nearest_alteration(target_pc - letter_pitch_class(out.letter));
    return out;
}

bool operator==(const SpelledPitch& a, const SpelledPitch& b) noexcept
{
    return mod(a.letter, 7) == mod(b.letter, 7) && a.alteration == b.alteration;
}
bool operator!=(const SpelledPitch& a, const SpelledPitch& b) noexcept { return !(a == b); }

// --- Modes ------------------------------------------------------------------

namespace {

struct ModeRow {
    Mode mode;
    const char* id;
    const char* display;
    std::vector<int> intervals;
};

const std::vector<ModeRow>& mode_table()
{
    static const std::vector<ModeRow> rows = {
        {Mode::Major, "major", "Major", {0, 2, 4, 5, 7, 9, 11}},
        {Mode::Minor, "minor", "Minor", {0, 2, 3, 5, 7, 8, 10}},
        {Mode::HarmonicMinor, "harmonic_minor", "Harmonic minor", {0, 2, 3, 5, 7, 8, 11}},
        {Mode::MelodicMinor, "melodic_minor", "Melodic minor", {0, 2, 3, 5, 7, 9, 11}},
        {Mode::Dorian, "dorian", "Dorian", {0, 2, 3, 5, 7, 9, 10}},
        {Mode::Phrygian, "phrygian", "Phrygian", {0, 1, 3, 5, 7, 8, 10}},
        {Mode::Lydian, "lydian", "Lydian", {0, 2, 4, 6, 7, 9, 11}},
        {Mode::Mixolydian, "mixolydian", "Mixolydian", {0, 2, 4, 5, 7, 9, 10}},
        {Mode::Aeolian, "aeolian", "Aeolian", {0, 2, 3, 5, 7, 8, 10}},
        {Mode::Locrian, "locrian", "Locrian", {0, 1, 3, 5, 6, 8, 10}},
        {Mode::PhrygianDominant, "phrygian_dominant", "Phrygian dominant", {0, 1, 4, 5, 7, 8, 10}},
        {Mode::DoubleHarmonic, "double_harmonic", "Double harmonic", {0, 1, 4, 5, 7, 8, 11}},
        {Mode::WholeTone, "whole_tone", "Whole tone", {0, 2, 4, 6, 8, 10}},
        {Mode::Octatonic, "octatonic", "Octatonic (half-whole)", {0, 1, 3, 4, 6, 7, 9, 10}},
        {Mode::MajorPentatonic, "major_pentatonic", "Major pentatonic", {0, 2, 4, 7, 9}},
        {Mode::MinorPentatonic, "minor_pentatonic", "Minor pentatonic", {0, 3, 5, 7, 10}},
        {Mode::Blues, "blues", "Blues", {0, 3, 5, 6, 7, 10}},
    };
    return rows;
}

const ModeRow& row(Mode m)
{
    for (const auto& r : mode_table())
        if (r.mode == m) return r;
    return mode_table().front();
}

}  // namespace

const char* to_string(Mode mode) noexcept { return row(mode).id; }
const char* display_name(Mode mode) noexcept { return row(mode).display; }
const std::vector<int>& mode_intervals(Mode mode) noexcept { return row(mode).intervals; }
bool is_heptatonic(Mode mode) noexcept { return row(mode).intervals.size() == 7; }

std::optional<Mode> mode_from_string(const std::string& text) noexcept
{
    std::string t = lower(text);
    std::replace(t.begin(), t.end(), ' ', '_');
    std::replace(t.begin(), t.end(), '-', '_');
    if (t == "ionian") return Mode::Major;
    if (t == "natural_minor") return Mode::Minor;
    for (const auto& r : mode_table())
        if (t == r.id) return r.mode;
    return std::nullopt;
}

Mode parent_heptatonic(Mode mode) noexcept
{
    switch (mode) {
        case Mode::WholeTone: return Mode::Lydian;
        case Mode::Octatonic: return Mode::Dorian;
        case Mode::MajorPentatonic: return Mode::Major;
        case Mode::MinorPentatonic:
        case Mode::Blues: return Mode::Minor;
        default: return mode;
    }
}

bool is_minor_family(Mode mode) noexcept { return mode_intervals(parent_heptatonic(mode))[2] == 3; }

// --- Key --------------------------------------------------------------------

std::vector<SpelledPitch> Key::scale() const
{
    const auto& iv = mode_intervals(mode);
    std::vector<SpelledPitch> out;
    if (iv.size() == 7) {
        for (int i = 0; i < 7; ++i) {
            SpelledPitch p;
            p.letter = mod(tonic.letter + i, 7);
            p.alteration = nearest_alteration(tonic.pitch_class() + iv[static_cast<std::size_t>(i)] -
                                              letter_pitch_class(p.letter));
            out.push_back(p);
        }
        return out;
    }
    // Non-heptatonic: reuse the parent scale's spelling where it matches,
    // otherwise spell by the key signature's direction.
    Key parent{tonic, parent_heptatonic(mode)};
    auto pscale = parent.scale();
    bool flats = parent.fifths() < 0;
    for (int interval : iv) {
        int pc = mod(tonic.pitch_class() + interval, 12);
        auto it = std::find_if(pscale.begin(), pscale.end(),
                               [&](const SpelledPitch& s) { return s.pitch_class() == pc; });
        if (it != pscale.end()) {
            out.push_back(*it);
            continue;
        }
        // Nearest letter in the requested direction.
        for (int l = 0; l < 7; ++l) {
            int alt = nearest_alteration(pc - letter_pitch_class(l));
            if ((flats && alt == -1) || (!flats && alt == 1)) {
                out.push_back({l, alt});
                break;
            }
        }
    }
    return out;
}

SpelledPitch Key::degree(int d) const
{
    Key parent{tonic, parent_heptatonic(mode)};
    return parent.scale()[static_cast<std::size_t>(mod(d - 1, 7))];
}

SpelledPitch Key::major_degree(int d) const
{
    return Key{tonic, Mode::Major}.scale()[static_cast<std::size_t>(mod(d - 1, 7))];
}

int Key::fifths() const noexcept
{
    static const int natural_fifths[7] = {0, 2, 4, -1, 1, 3, 5};  // C D E F G A B
    int major = natural_fifths[mod(tonic.letter, 7)] + tonic.alteration * 7;
    int offset = 0;
    switch (parent_heptatonic(mode)) {
        case Mode::Lydian: offset = 1; break;
        case Mode::Mixolydian: offset = -1; break;
        case Mode::Dorian: offset = -2; break;
        case Mode::Minor:
        case Mode::Aeolian:
        case Mode::HarmonicMinor:
        case Mode::MelodicMinor: offset = -3; break;
        case Mode::Phrygian:
        case Mode::PhrygianDominant:
        case Mode::DoubleHarmonic: offset = -4; break;
        case Mode::Locrian: offset = -5; break;
        default: offset = 0;
    }
    return std::clamp(major + offset, -7, 7);
}

std::string Key::name() const
{
    std::string m = to_string(mode);
    std::replace(m.begin(), m.end(), '_', ' ');
    return tonic.name() + " " + m;
}

std::optional<Key> Key::parse(const std::string& text)
{
    std::string t = text;
    while (!t.empty() && std::isspace(static_cast<unsigned char>(t.back()))) t.pop_back();
    auto space = t.find(' ');
    if (space == std::string::npos) {
        if (auto p = SpelledPitch::parse(t)) return Key{*p, Mode::Major};
        if (t.size() >= 2 && t.back() == 'm')
            if (auto p = SpelledPitch::parse(t.substr(0, t.size() - 1))) return Key{*p, Mode::Minor};
        return std::nullopt;
    }
    auto tonic = SpelledPitch::parse(t.substr(0, space));
    auto m = mode_from_string(t.substr(space + 1));
    if (!tonic || !m) return std::nullopt;
    return Key{*tonic, *m};
}

bool Key::contains(int pc) const
{
    pc = mod(pc, 12);
    for (int i : mode_intervals(mode))
        if (mod(tonic.pitch_class() + i, 12) == pc) return true;
    return false;
}

// --- Tuning -----------------------------------------------------------------

TuningSystem TuningSystem::equal_temperament() { return TuningSystem{}; }

TuningSystem TuningSystem::quarter_comma_meantone()
{
    TuningSystem t;
    t.name = "Quarter-comma meantone";
    t.cents_offset = {0.0, -24.0, -6.8, 10.3, -13.7, 3.4, -20.5, -3.4, -27.4, -10.3, 6.8, -17.1};
    return t;
}

TuningSystem TuningSystem::werckmeister_iii()
{
    TuningSystem t;
    t.name = "Werckmeister III";
    t.cents_offset = {0.0, -9.8, -7.8, -5.9, -9.8, -2.0, -11.7, -3.9, -7.8, -11.7, -3.9, -7.8};
    return t;
}

double TuningSystem::frequency(int midi_note) const noexcept
{
    // A4 stays at the reference whatever the temperament.
    double off = cents_offset[static_cast<std::size_t>(mod(midi_note, 12))] - cents_offset[9];
    return reference_a4_hz * std::pow(2.0, (midi_note - 69 + off / 100.0) / 12.0);
}

PitchCollection::Realized PitchCollection::realize(int tonic_midi, int step) const
{
    Realized r;
    if (cents.empty()) {
        r.midi_note = tonic_midi;
        return r;
    }
    int n = static_cast<int>(cents.size());
    int octave = step >= 0 ? step / n : -((-step + n - 1) / n);
    int idx = step - octave * n;
    double total = octave * 1200.0 + cents[static_cast<std::size_t>(idx)];
    long semis = std::lround(total / 100.0);
    r.midi_note = tonic_midi + static_cast<int>(semis);
    r.bend_cents = total - static_cast<double>(semis) * 100.0;
    return r;
}

PitchCollection PitchCollection::from_mode(Mode mode)
{
    PitchCollection c;
    c.name = display_name(mode);
    for (int i : mode_intervals(mode)) c.cents.push_back(i * 100.0);
    return c;
}

}  // namespace vcc::theory
