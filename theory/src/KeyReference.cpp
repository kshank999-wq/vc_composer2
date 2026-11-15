#include "vcc/theory/KeyReference.h"

#include <algorithm>

namespace vcc::theory {

namespace {

SpelledPitch sp(int letter, int alteration)
{
    SpelledPitch p;
    p.letter = letter;
    p.alteration = alteration;
    return p;
}

int mod(int a, int m) { return ((a % m) + m) % m; }

/// The major tonic for a key signature: C plus `fifths` fifths, spelled.
SpelledPitch major_for_fifths(int fifths)
{
    SpelledPitch p = sp(0, 0);
    if (fifths >= 0)
        for (int i = 0; i < fifths; ++i) p = p.transposed(4, 7);
    else
        for (int i = 0; i < -fifths; ++i) p = p.transposed(3, 5);
    return p;
}

SpelledPitch relative_minor_of(const SpelledPitch& major) { return major.transposed(5, 9); }

}  // namespace

const std::vector<CirclePosition>& circle_of_fifths()
{
    static const std::vector<CirclePosition> circle = [] {
        // Clockwise from C: sharps to six, then flats from five back to one.
        const int order[12] = {0, 1, 2, 3, 4, 5, 6, -5, -4, -3, -2, -1};
        std::vector<CirclePosition> out;
        for (int f : order) {
            CirclePosition p;
            p.fifths = f;
            p.major = major_for_fifths(f);
            p.minor = relative_minor_of(p.major);
            // The enharmonic spellings a circle shows: B/Cb, F#/Gb, Db/C#.
            if (f == 5 || f == 6 || f == -5) {
                const int other = f > 0 ? f - 12 : f + 12;
                p.major_enharmonic = major_for_fifths(other);
                p.minor_enharmonic = relative_minor_of(*p.major_enharmonic);
            }
            out.push_back(p);
        }
        return out;
    }();
    return circle;
}

int circle_index(const Key& key) noexcept
{
    const int f = key.fifths();
    const int norm = f > 6 ? f - 12 : f < -5 ? f + 12 : f;
    const auto& c = circle_of_fifths();
    for (std::size_t i = 0; i < c.size(); ++i)
        if (c[i].fifths == norm) return static_cast<int>(i);
    return 0;
}

Key key_for_signature(int fifths, bool minor)
{
    const SpelledPitch major = major_for_fifths(std::clamp(fifths, -7, 7));
    return minor ? Key{relative_minor_of(major), Mode::Minor} : Key{major, Mode::Major};
}

std::string signature_text(int fifths)
{
    if (fifths == 0) return "no sharps or flats";
    const int n = std::abs(fifths);
    return std::to_string(n) + (fifths > 0 ? (n == 1 ? " sharp" : " sharps") : (n == 1 ? " flat" : " flats"));
}

namespace {

/// The chord a stack of scale notes makes, and its Roman numeral.
DiatonicChord diatonic_chord(const std::vector<SpelledPitch>& notes, int degree, bool seventh)
{
    static const char* const kRoman[] = {"I", "II", "III", "IV", "V", "VI", "VII"};
    const auto& root = notes[static_cast<std::size_t>(degree - 1)];
    auto at = [&](int step) { return notes[static_cast<std::size_t>((degree - 1 + step) % 7)]; };
    auto semis = [&](const SpelledPitch& p) { return mod(p.pitch_class() - root.pitch_class(), 12); };
    const int third = semis(at(2)), fifth = semis(at(4)), sev = semis(at(6));
    std::string id, suffix;
    bool upper = third == 4;
    if (!seventh) {
        if (third == 4 && fifth == 7) id = "maj";
        else if (third == 3 && fifth == 7) id = "m";
        else if (third == 3 && fifth == 6) { id = "dim"; suffix = "\xC2\xB0"; }
        else if (third == 4 && fifth == 8) { id = "aug"; suffix = "+"; }
        else id = "maj";
    } else {
        if (third == 4 && fifth == 7 && sev == 11) { id = "maj7"; suffix = "maj7"; }
        else if (third == 4 && fifth == 7 && sev == 10) { id = "7"; suffix = "7"; }
        else if (third == 3 && fifth == 7 && sev == 10) { id = "m7"; suffix = "7"; }
        else if (third == 3 && fifth == 6 && sev == 10) { id = "m7b5"; suffix = "\xC3\xB8" "7"; }
        else if (third == 3 && fifth == 6 && sev == 9) { id = "dim7"; suffix = "\xC2\xB0" "7"; }
        else if (third == 3 && fifth == 7 && sev == 11) { id = "mmaj7"; suffix = "(maj7)"; }
        else if (third == 4 && fifth == 8 && sev == 11) { id = "augmaj7"; suffix = "+maj7"; }
        else if (third == 4 && fifth == 8 && sev == 10) { id = "aug7"; suffix = "+7"; }
        else { id = "7"; suffix = "7"; }
    }
    DiatonicChord c;
    c.degree = degree;
    std::string numeral = kRoman[degree - 1];
    if (!upper)
        for (auto& ch : numeral) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    c.roman = numeral + suffix;
    c.chord.root = root;
    c.chord.quality = *find_quality(id);
    return c;
}

}  // namespace

namespace {

/// Adds the chord on `degree` of `scale` unless the key shown has it.
void add_borrowed(std::vector<BorrowedChord>& out, const KeyReference& r, const std::vector<SpelledPitch>& scale, int degree,
                  bool seventh, const std::string& source, const std::string& name)
{
    if (scale.size() != 7) return;
    const bool minor = is_minor_family(r.key.mode);
    const auto home_natural = r.key.scale();
    if (home_natural.size() != 7) return;
    const auto& home = seventh ? r.sevenths : r.triads;
    auto c = diatonic_chord(scale, degree, seventh);
    // Already in the key shown (same root, same quality): not borrowed.
    for (const auto& h : home)
        if (h.chord.root.pitch_class() == c.chord.root.pitch_class() && h.chord.quality.id == c.chord.quality.id) return;
    for (const auto& b : out)
        if (b.chord.root.pitch_class() == c.chord.root.pitch_class() && b.chord.quality.id == c.chord.quality.id) return;
    // Relative to the home key: a root lowered or raised from the key's own
    // degree takes a flat or sharp (a minor key's raised 6th and 7th are
    // its own, as harmonic/melodic minor writes them).
    const int diff = mod(c.chord.root.pitch_class() - home_natural[static_cast<std::size_t>(degree - 1)].pitch_class() + 6, 12) - 6;
    std::string prefix;
    if (diff < 0) prefix = "\xE2\x99\xAD";  // flat
    if (diff > 0 && !(minor && (degree == 6 || degree == 7))) prefix = "\xE2\x99\xAF";  // sharp
    BorrowedChord b;
    b.degree = degree;
    b.roman = prefix + c.roman;
    b.chord = c.chord;
    b.source = source;
    b.name = name;
    out.push_back(b);
}

Mode mode_of(BorrowSource s)
{
    switch (s) {
        case BorrowSource::Dorian: return Mode::Dorian;
        case BorrowSource::Phrygian: return Mode::Phrygian;
        case BorrowSource::Lydian: return Mode::Lydian;
        case BorrowSource::Mixolydian: return Mode::Mixolydian;
        case BorrowSource::Locrian: return Mode::Locrian;
        case BorrowSource::HarmonicMinor: return Mode::HarmonicMinor;
        case BorrowSource::MelodicMinor: return Mode::MelodicMinor;
        case BorrowSource::Parallel: break;
    }
    return Mode::Major;
}

}  // namespace

const std::vector<BorrowSource>& borrow_sources()
{
    static const std::vector<BorrowSource> all = {BorrowSource::Parallel,      BorrowSource::Dorian,  BorrowSource::Phrygian,
                                                  BorrowSource::Lydian,        BorrowSource::Mixolydian, BorrowSource::Locrian,
                                                  BorrowSource::HarmonicMinor, BorrowSource::MelodicMinor};
    return all;
}

std::string borrow_source_name(BorrowSource s, bool minor_home)
{
    switch (s) {
        case BorrowSource::Parallel: return minor_home ? "Parallel major" : "Parallel minor";
        case BorrowSource::Dorian: return "Dorian";
        case BorrowSource::Phrygian: return "Phrygian";
        case BorrowSource::Lydian: return "Lydian";
        case BorrowSource::Mixolydian: return "Mixolydian";
        case BorrowSource::Locrian: return "Locrian";
        case BorrowSource::HarmonicMinor: return "Harmonic minor";
        case BorrowSource::MelodicMinor: return "Melodic minor";
    }
    return {};
}

std::vector<BorrowedChord> borrowed_chords(const KeyReference& r, BorrowSource source, bool seventh)
{
    std::vector<BorrowedChord> out;
    const bool minor = is_minor_family(r.key.mode);
    const std::string tonic = r.key.tonic.display_name();
    if (source == BorrowSource::Parallel) {
        const auto parallel_notes = Key{r.key.tonic, minor ? Mode::Major : Mode::Minor}.scale();
        const auto phrygian_notes = Key{r.key.tonic, Mode::Phrygian}.scale();
        const std::string from = tonic + (minor ? " major" : " minor");
        for (int d = 1; d <= 7; ++d) {
            if (d == 2) add_borrowed(out, r, phrygian_notes, 2, seventh, tonic + " Phrygian", "Neapolitan");
            add_borrowed(out, r, parallel_notes, d, seventh, from, minor && d == 1 ? "Picardy third" : "");
        }
        return out;
    }
    const auto notes = Key{r.key.tonic, mode_of(source)}.scale();
    const std::string from = tonic + " " + borrow_source_name(source, minor);
    for (int d = 1; d <= 7; ++d) add_borrowed(out, r, notes, d, seventh, from, "");
    return out;
}

KeyReference key_reference(const Key& key, MinorForm form)
{
    KeyReference r;
    const bool minor = is_minor_family(key.mode);
    r.fifths = key.fifths();
    r.key.tonic = key.tonic;
    r.key.mode = minor ? Mode::Minor : Mode::Major;
    r.form = minor ? form : MinorForm::Natural;
    // The key signature's accidentals, in order.
    static const int kSharps[7] = {3, 0, 4, 1, 5, 2, 6};  // F C G D A E B
    static const int kFlats[7] = {6, 2, 5, 1, 4, 0, 3};   // B E A D G C F
    for (int i = 0; i < std::min(7, std::abs(r.fifths)); ++i)
        r.signature.push_back(r.fifths > 0 ? sp(kSharps[i], 1) : sp(kFlats[i], -1));

    // The scale in the chosen form.
    Key shown = r.key;
    if (minor && (form == MinorForm::Harmonic)) shown.mode = Mode::HarmonicMinor;
    if (minor && (form == MinorForm::MelodicAscending)) shown.mode = Mode::MelodicMinor;
    const auto natural = r.key.scale();
    const auto notes = shown.scale();
    static const char* const kFunctions[] = {"Tonic", "Supertonic", "Mediant", "Subdominant", "Dominant", "Submediant"};
    for (int d = 1; d <= 7 && d <= static_cast<int>(notes.size()); ++d) {
        ScaleDegree s;
        s.degree = d;
        s.note = notes[static_cast<std::size_t>(d - 1)];
        s.altered = !(s.note == natural[static_cast<std::size_t>(d - 1)]);
        if (d <= 6) s.function = kFunctions[d - 1];
        else s.function = mod(s.note.pitch_class() - key.tonic.pitch_class(), 12) == 11 ? "Leading tone" : "Subtonic";
        r.scale.push_back(s);
    }
    std::vector<std::string> raised;
    for (const auto& s : r.scale)
        if (s.altered) raised.push_back("raised " + std::to_string(s.degree) + (s.degree == 6 ? "th" : "th") + " (" + s.note.name() + ")");
    for (std::size_t i = 0; i < raised.size(); ++i) r.altered_note += (i ? ", " : "") + raised[i];
    if (!r.altered_note.empty()) r.altered_note[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(r.altered_note[0])));

    if (notes.size() == 7) {
        for (int d = 1; d <= 7; ++d) {
            for (bool seventh : {false, true}) {
                auto c = diatonic_chord(notes, d, seventh);
                for (int step : {0, 2, 4, 6}) {
                    if (step == 6 && !seventh) continue;
                    if (r.scale[static_cast<std::size_t>((d - 1 + step) % 7)].altered) c.uses_altered = true;
                }
                (seventh ? r.sevenths : r.triads).push_back(c);
            }
        }
    }
    // Borrowed chords: from the parallel key, and the Neapolitan.
    r.borrowed_triads = borrowed_chords(r, BorrowSource::Parallel, false);
    r.borrowed_sevenths = borrowed_chords(r, BorrowSource::Parallel, true);
    // The keys around it.
    r.relative = minor ? Key{key.tonic.transposed(2, 3), Mode::Major} : Key{relative_minor_of(key.tonic), Mode::Minor};
    r.parallel = Key{key.tonic, minor ? Mode::Major : Mode::Minor};
    r.dominant = Key{key.tonic.transposed(4, 7), r.key.mode};
    r.subdominant = Key{key.tonic.transposed(3, 5), r.key.mode};
    return r;
}

}  // namespace vcc::theory
