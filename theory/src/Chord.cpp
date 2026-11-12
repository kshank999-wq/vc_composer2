#include "vcc/theory/Chord.h"

#include <algorithm>
#include <cctype>
#include <map>

namespace vcc::theory {

namespace {

using I = ChordInterval;

const I R{1, 0}, m3{3, 3}, M3{3, 4}, P5{5, 7}, d5{5, 6}, A5{5, 8}, M2{2, 2}, P4{4, 5}, M6{6, 9},
    m7{7, 10}, M7{7, 11}, d7{7, 9}, M9{9, 14}, b9{9, 13}, s9{9, 15}, P11{11, 17}, s11{11, 18},
    M13{13, 21}, b13{13, 20};

}  // namespace

bool ChordQuality::is_seventh_or_more() const noexcept
{
    return std::any_of(tones.begin(), tones.end(), [](const ChordInterval& t) { return t.degree >= 7; });
}

const std::vector<ChordQuality>& chord_qualities()
{
    static const std::vector<ChordQuality> table = {
        {"maj", "", {R, M3, P5}},
        {"m", "m", {R, m3, P5}},
        {"dim", "dim", {R, m3, d5}},
        {"aug", "aug", {R, M3, A5}},
        {"sus2", "sus2", {R, M2, P5}},
        {"sus4", "sus4", {R, P4, P5}},
        {"6", "6", {R, M3, P5, M6}},
        {"m6", "m6", {R, m3, P5, M6}},
        {"69", "6/9", {R, M3, P5, M6, M9}},
        {"7", "7", {R, M3, P5, m7}},
        {"maj7", "maj7", {R, M3, P5, M7}},
        {"m7", "m7", {R, m3, P5, m7}},
        {"m7b5", "m7b5", {R, m3, d5, m7}},
        {"dim7", "dim7", {R, m3, d5, d7}},
        {"mmaj7", "m(maj7)", {R, m3, P5, M7}},
        {"aug7", "7#5", {R, M3, A5, m7}},
        {"augmaj7", "maj7#5", {R, M3, A5, M7}},
        {"7sus4", "7sus4", {R, P4, P5, m7}},
        {"add9", "add9", {R, M3, P5, M9}},
        {"madd9", "madd9", {R, m3, P5, M9}},
        {"9", "9", {R, M3, P5, m7, M9}},
        {"maj9", "maj9", {R, M3, P5, M7, M9}},
        {"m9", "m9", {R, m3, P5, m7, M9}},
        {"7b9", "7b9", {R, M3, P5, m7, b9}},
        {"7#9", "7#9", {R, M3, P5, m7, s9}},
        {"11", "11", {R, P5, m7, M9, P11}},
        {"m11", "m11", {R, m3, P5, m7, M9, P11}},
        {"maj7#11", "maj7#11", {R, M3, P5, M7, s11}},
        {"13", "13", {R, M3, m7, M9, M13}},
        {"maj13", "maj13", {R, M3, M7, M9, M13}},
        {"m13", "m13", {R, m3, m7, M9, M13}},
        {"7alt", "7alt", {R, M3, m7, b9, s9, b13}},
    };
    return table;
}

const ChordQuality* find_quality(const std::string& id) noexcept
{
    for (const auto& q : chord_qualities())
        if (q.id == id) return &q;
    return nullptr;
}

// --- Chord ------------------------------------------------------------------

std::vector<ChordInterval> Chord::intervals() const
{
    std::vector<ChordInterval> out;
    for (const auto& t : quality.tones)
        if (std::find(omit_degrees.begin(), omit_degrees.end(), t.degree) == omit_degrees.end() || t.degree == 1)
            out.push_back(t);
    for (const auto& a : added) out.push_back(a);
    return out;
}

std::vector<SpelledPitch> Chord::tones() const
{
    std::vector<SpelledPitch> out;
    for (const auto& t : intervals()) out.push_back(root.transposed(t.degree - 1, t.semitones));
    return out;
}

SpelledPitch Chord::bass_note() const
{
    if (bass) return *bass;
    auto t = tones();
    if (t.empty()) return root;
    std::size_t idx = static_cast<std::size_t>(std::clamp(inversion, 0, 3));
    return t[std::min(idx, t.size() - 1)];
}

std::string Chord::name() const
{
    std::string s = root.name() + quality.symbol;
    for (const auto& a : added) {
        if (a.degree == 9) s += "(add9)";
        else if (a.degree == 6) s += "(add6)";
        else s += "(add" + std::to_string(a.degree) + ")";
    }
    for (int d : omit_degrees)
        if (d != 1) s += "(no" + std::to_string(d) + ")";
    SpelledPitch b = bass_note();
    if (b != root) s += "/" + b.name();
    return s;
}

std::string Chord::display_name() const
{
    std::string sym = quality.symbol;
    if (quality.id == "m7b5") sym = "\xC3\xB8" "7";
    else if (quality.id == "dim") sym = "\xC2\xB0";
    else if (quality.id == "dim7") sym = "\xC2\xB0" "7";
    else if (quality.id == "aug") sym = "+";
    std::string out = root.display_name();
    for (char c : sym) {
        if (c == 'b') out += "\xE2\x99\xAD";
        else if (c == '#') out += "\xE2\x99\xAF";
        else out += c;
    }
    SpelledPitch b = bass_note();
    if (b != root) out += "/" + b.display_name();
    return out;
}

// --- Chord symbols ----------------------------------------------------------

std::optional<Chord> parse_chord_symbol(const std::string& text)
{
    if (text.empty()) return std::nullopt;
    std::string body = text, bass_text;
    auto slash = text.find('/');
    // "6/9" is a quality, not a slash chord.
    if (slash != std::string::npos && !(slash > 0 && text[slash - 1] == '6' && slash + 1 < text.size() && text[slash + 1] == '9')) {
        body = text.substr(0, slash);
        bass_text = text.substr(slash + 1);
    }
    std::size_t i = 1;
    while (i < body.size() && (body[i] == '#' || body[i] == 'b')) ++i;
    auto root = SpelledPitch::parse(body.substr(0, i));
    if (!root) return std::nullopt;
    std::string suffix = body.substr(i);
    static const std::map<std::string, std::string> aliases = {
        {"", "maj"}, {"M", "maj"}, {"maj", "maj"}, {"m", "m"}, {"min", "m"}, {"-", "m"},
        {"dim", "dim"}, {"o", "dim"}, {"\xC2\xB0", "dim"}, {"aug", "aug"}, {"+", "aug"},
        {"sus2", "sus2"}, {"sus4", "sus4"}, {"sus", "sus4"}, {"6", "6"}, {"m6", "m6"},
        {"6/9", "69"}, {"69", "69"}, {"7", "7"}, {"dom7", "7"}, {"maj7", "maj7"}, {"M7", "maj7"},
        {"\xCE\x94", "maj7"}, {"\xCE\x94" "7", "maj7"}, {"m7", "m7"}, {"min7", "m7"}, {"-7", "m7"},
        {"m7b5", "m7b5"}, {"\xC3\xB8", "m7b5"}, {"\xC3\xB8" "7", "m7b5"}, {"-7b5", "m7b5"},
        {"dim7", "dim7"}, {"o7", "dim7"}, {"\xC2\xB0" "7", "dim7"}, {"m(maj7)", "mmaj7"},
        {"mMaj7", "mmaj7"}, {"mmaj7", "mmaj7"}, {"m/maj7", "mmaj7"}, {"7#5", "aug7"},
        {"aug7", "aug7"}, {"+7", "aug7"}, {"maj7#5", "augmaj7"}, {"7sus4", "7sus4"}, {"7sus", "7sus4"},
        {"add9", "add9"}, {"madd9", "madd9"}, {"9", "9"}, {"maj9", "maj9"}, {"M9", "maj9"},
        {"m9", "m9"}, {"7b9", "7b9"}, {"7#9", "7#9"}, {"11", "11"}, {"m11", "m11"},
        {"maj7#11", "maj7#11"}, {"13", "13"}, {"maj13", "maj13"}, {"m13", "m13"}, {"7alt", "7alt"},
        {"alt", "7alt"},
    };
    auto it = aliases.find(suffix);
    if (it == aliases.end()) return std::nullopt;
    Chord c;
    c.root = *root;
    c.quality = *find_quality(it->second);
    if (!bass_text.empty()) {
        auto b = SpelledPitch::parse(bass_text);
        if (!b) return std::nullopt;
        // A bass that is a chord tone is an inversion; otherwise a slash bass.
        auto tones = c.tones();
        auto pos = std::find(tones.begin(), tones.end(), *b);
        if (pos != tones.end() && pos - tones.begin() <= 3) c.inversion = static_cast<int>(pos - tones.begin());
        else c.bass = *b;
    }
    return c;
}

// --- Roman numerals -----------------------------------------------------------

namespace {

bool starts_with(const std::string& s, std::size_t pos, const char* prefix)
{
    return s.compare(pos, std::char_traits<char>::length(prefix), prefix) == 0;
}

}  // namespace

std::optional<RomanNumeral> parse_roman(const std::string& text, std::string* error)
{
    auto fail = [&](const std::string& why) -> std::optional<RomanNumeral> {
        if (error) *error = "'" + text + "': " + why;
        return std::nullopt;
    };
    RomanNumeral r;
    r.text = text;
    std::string body = text;
    auto slash = body.find('/');
    if (slash != std::string::npos) {
        r.applied_to = body.substr(slash + 1);
        body = body.substr(0, slash);
        if (r.applied_to->empty()) return fail("empty applied target");
    }
    auto at = body.find('@');
    if (at != std::string::npos) {
        std::string d = body.substr(at + 1);
        if (d.size() != 1 || d[0] < '1' || d[0] > '7') return fail("pedal must be @1..@7");
        r.pedal_degree = d[0] - '0';
        body = body.substr(0, at);
    }
    std::size_t i = 0;
    while (i < body.size()) {
        if (body[i] == 'b') { --r.accidental; r.has_accidental = true; ++i; }
        else if (body[i] == '#') { ++r.accidental; r.has_accidental = true; ++i; }
        else if (starts_with(body, i, "\xE2\x99\xAD")) { --r.accidental; r.has_accidental = true; i += 3; }
        else if (starts_with(body, i, "\xE2\x99\xAF")) { ++r.accidental; r.has_accidental = true; i += 3; }
        else break;
    }
    static const std::pair<const char*, int> numerals[] = {
        {"VII", 7}, {"VI", 6}, {"V", 5}, {"IV", 4}, {"III", 3}, {"II", 2}, {"I", 1},
        {"vii", 7}, {"vi", 6}, {"v", 5}, {"iv", 4}, {"iii", 3}, {"ii", 2}, {"i", 1}};
    bool matched = false;
    for (const auto& n : numerals) {
        if (starts_with(body, i, n.first)) {
            r.degree = n.second;
            r.upper = std::isupper(static_cast<unsigned char>(n.first[0])) != 0;
            i += std::char_traits<char>::length(n.first);
            matched = true;
            break;
        }
    }
    if (!matched) return fail("no Roman numeral");

    enum class Mark { None, Dim, HalfDim, Aug } mark = Mark::None;
    if (i < body.size() && body[i] == 'o') { mark = Mark::Dim; ++i; }
    else if (starts_with(body, i, "\xC2\xB0")) { mark = Mark::Dim; i += 2; }
    else if (starts_with(body, i, "\xC3\xB8")) { mark = Mark::HalfDim; i += 2; }
    else if (i < body.size() && body[i] == '+') { mark = Mark::Aug; ++i; }

    std::string fig = body.substr(i);
    const bool up = r.upper;
    std::string q;
    auto seventh = [&]() -> std::string {
        switch (mark) {
            case Mark::Dim: return "dim7";
            case Mark::HalfDim: return "m7b5";
            case Mark::Aug: return "aug7";
            default: return up ? "7" : "m7";
        }
    };
    auto triad = [&]() -> std::string {
        switch (mark) {
            case Mark::Dim: return "dim";
            case Mark::HalfDim: return "m7b5";
            case Mark::Aug: return "aug";
            default: return up ? "maj" : "m";
        }
    };
    if (fig.empty()) q = triad();
    else if (fig == "6") { q = triad(); r.inversion = 1; }
    else if (fig == "64") { q = triad(); r.inversion = 2; }
    else if (fig == "7") q = seventh();
    else if (fig == "65") { q = seventh(); r.inversion = 1; }
    else if (fig == "43") { q = seventh(); r.inversion = 2; }
    else if (fig == "42" || fig == "2") { q = seventh(); r.inversion = 3; }
    else if (fig == "maj7") q = mark == Mark::Aug ? "augmaj7" : (up ? "maj7" : "mmaj7");
    else if (fig == "9") q = up ? "9" : "m9";
    else if (fig == "maj9") q = "maj9";
    else if (fig == "11") q = up ? "11" : "m11";
    else if (fig == "13") q = up ? "13" : "m13";
    else if (fig == "maj13") q = "maj13";
    else if (fig == "add9") q = up ? "add9" : "madd9";
    else if (fig == "add6") q = up ? "6" : "m6";
    else if (fig == "sus2") q = "sus2";
    else if (fig == "sus4" || fig == "sus") q = "sus4";
    else if (fig == "7sus4") q = "7sus4";
    else if (fig == "7alt" || fig == "alt") q = "7alt";
    else if (fig == "7b9") q = "7b9";
    else if (fig == "7#9") q = "7#9";
    else if (fig == "maj7#11") q = "maj7#11";
    else return fail("unknown figure '" + fig + "'");
    if (!up && (q == "7alt" || q == "7b9" || q == "7#9" || q == "maj9" || q == "maj13"))
        return fail("'" + fig + "' needs an upper-case numeral");
    r.quality_id = q;
    return r;
}

std::optional<Chord> chord_from_roman(const std::string& numeral, const Key& key, std::string* error)
{
    auto r = parse_roman(numeral, error);
    if (!r) return std::nullopt;
    Key context = key;
    if (r->applied_to) {
        auto target = chord_from_roman(*r->applied_to, key, error);
        if (!target) return std::nullopt;
        bool minor_target = !target->quality.tones.empty() &&
                            std::any_of(target->quality.tones.begin(), target->quality.tones.end(),
                                        [](const ChordInterval& t) { return t.degree == 3 && t.semitones == 3; });
        context = Key{target->root, minor_target ? Mode::HarmonicMinor : Mode::Major};
    }
    Chord c;
    c.root = r->has_accidental ? context.major_degree(r->degree).transposed(0, r->accidental)
                               : context.degree(r->degree);
    c.quality = *find_quality(r->quality_id);
    c.inversion = r->inversion;
    if (r->pedal_degree) {
        SpelledPitch b = key.degree(*r->pedal_degree);
        if (b != c.root) c.bass = b;
    }
    return c;
}

// --- Diatonic chords ----------------------------------------------------------

namespace {

std::string degree_numeral(int degree, bool upper)
{
    static const char* up[] = {"I", "II", "III", "IV", "V", "VI", "VII"};
    static const char* lo[] = {"i", "ii", "iii", "iv", "v", "vi", "vii"};
    const int i = ((degree - 1) % 7 + 7) % 7;
    const char* text = upper ? up[i] : lo[i];
    return std::string(text);
}

bool same_tones(const std::vector<ChordInterval>& a, const std::vector<ChordInterval>& b)
{
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (a[i].degree != b[i].degree || a[i].semitones != b[i].semitones) return false;
    return true;
}

}  // namespace

std::vector<Chord> diatonic_chords(const Key& key, int size)
{
    std::vector<Chord> out;
    if (!is_heptatonic(key.mode)) return out;
    size = std::clamp(size, 3, 5);
    auto scale = key.scale();
    for (int d = 0; d < 7; ++d) {
        Chord c;
        c.root = scale[static_cast<std::size_t>(d)];
        std::vector<ChordInterval> tones;
        for (int k = 0; k < size; ++k) {
            int step = 2 * k;  // 0,2,4,6,8 scale steps above the root
            SpelledPitch p = scale[static_cast<std::size_t>((d + step) % 7)];
            int semis = (p.pitch_class() - c.root.pitch_class() + 12) % 12;
            if (step >= 7) semis += 12;
            tones.push_back({step + 1, semis});
        }
        const ChordQuality* match = nullptr;
        for (const auto& q : chord_qualities())
            if (same_tones(q.tones, tones)) { match = &q; break; }
        if (match) {
            c.quality = *match;
        } else {
            // A diatonic stack the table does not name (e.g. a phrygian i9
            // with a flat ninth): name it from its seventh chord plus tension.
            std::vector<ChordInterval> base(tones.begin(), tones.begin() + std::min<std::size_t>(4, tones.size()));
            std::string sym = "(?)";
            for (const auto& q : chord_qualities())
                if (same_tones(q.tones, base)) { sym = q.symbol; break; }
            if (tones.size() == 5) {
                int ninth = tones[4].semitones;
                sym += ninth == 13 ? "(b9)" : ninth == 15 ? "(#9)" : "(9)";
            }
            c.quality = ChordQuality{"custom", sym, tones};
        }
        out.push_back(c);
    }
    return out;
}

std::string roman_label(int degree, const ChordQuality& q)
{
    bool minor_third = std::any_of(q.tones.begin(), q.tones.end(),
                                   [](const ChordInterval& t) { return t.degree == 3 && t.semitones == 3; });
    std::string n = degree_numeral(degree, !minor_third);
    if (q.id == "maj" || q.id == "m") return n;
    if (q.id == "dim") return n + "\xC2\xB0";
    if (q.id == "aug") return n + "+";
    if (q.id == "m7b5") return n + "\xC3\xB8" "7";
    if (q.id == "dim7") return n + "\xC2\xB0" "7";
    if (q.id == "7" || q.id == "m7") return n + "7";
    if (q.id == "maj7" || q.id == "mmaj7") return n + "maj7";
    if (q.id == "9" || q.id == "m9") return n + "9";
    std::string sym = q.symbol;
    if (!sym.empty() && sym[0] == 'm' && sym.rfind("maj", 0) != 0) sym = sym.substr(1);
    return n + sym;
}

}  // namespace vcc::theory
