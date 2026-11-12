// MusicXML import: score-partwise (or score-timewise) -> scores + maps.
//
// Pass 1 reads every part's measures into raw events at exact rational
// positions (quarter notes), following <divisions>, <backup>, <forward> and
// <chord/> the way MusicXML defines them. Pass 2 lays out the measures for
// all parts at once - the project has one meter map - deciding pickups,
// short implicit bars and overfull bars. Pass 3 turns each part's raw
// events into a notation Score on that layout.
#include "vcc/interchange/MusicXml.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>
#include <optional>
#include <set>
#include <tuple>

#include "MusicXmlCommon.h"
#include "vcc/interchange/Xml.h"

namespace vcc::interchange {

namespace nt = vcc::notation;

namespace {

// --- Exact positions ---------------------------------------------------------------

/// A non-negative rational number of quarter notes.
struct Rational {
    long long num = 0;
    long long den = 1;

    static Rational of(long long n, long long d)
    {
        if (d <= 0) d = 1;
        Rational r{n, d};
        r.reduce();
        return r;
    }
    void reduce()
    {
        const long long g = detail::gcd_ll(num, den);
        if (g > 1) {
            num /= g;
            den /= g;
        }
        if (den > (1LL << 40)) {  // Pathological divisions: keep it bounded.
            num = static_cast<long long>(std::llround(static_cast<double>(num) * kTicksPerQuarter /
                                                      static_cast<double>(den)));
            den = kTicksPerQuarter;
            const long long g2 = detail::gcd_ll(num, den);
            if (g2 > 1) {
                num /= g2;
                den /= g2;
            }
        }
    }
    Rational operator+(const Rational& o) const
    {
        const long long g = detail::gcd_ll(den, o.den);
        return of(num * (o.den / g) + o.num * (den / g), den / g * o.den);
    }
    Rational operator-(const Rational& o) const { return *this + Rational{-o.num, o.den}; }
    bool operator<(const Rational& o) const { return num * o.den < o.num * den; }
    bool operator>(const Rational& o) const { return o < *this; }
    bool negative() const { return num < 0; }
    bool zero() const { return num == 0; }

    /// Nearest score tick.
    Ticks ticks() const
    {
        const long long scaled = num * kTicksPerQuarter;
        return den == 1 ? scaled : (scaled >= 0 ? (scaled + den / 2) / den : -((-scaled + den / 2) / den));
    }
    bool exact_ticks() const { return (num * kTicksPerQuarter) % den == 0; }
};

// --- Raw events -----------------------------------------------------------------------

struct RawTuplet {
    bool start = true;
    int number = 1;
    int actual = 0;  ///< From <tuplet-actual>, 0 when not given.
    int normal = 0;
};

struct RawNote {
    Rational offset;
    Rational dur;
    int staff = 1;
    std::string voice = "1";
    bool chord = false, grace = false, slash = false, cue = false;
    bool rest = false, measure_rest = false, invisible = false, unpitched = false;
    nt::Pitch pitch;
    std::string instrument;
    std::optional<nt::NoteValue> type;
    bool has_type = false;
    int dots = 0;
    int tm_actual = 1, tm_normal = 1;
    bool tie_start = false, tie_stop = false;
    nt::ArticulationMask arts = 0;
    std::vector<std::pair<std::string, int>> slurs;  ///< (type, number)
    std::vector<RawTuplet> tuplets;
    std::vector<std::pair<nt::DynamicLevel, std::string>> dynamics;
};

struct RawDirection {
    Rational offset;
    int staff = 1;
    std::string voice;
    std::vector<std::pair<nt::DynamicLevel, std::string>> dynamics;
    std::vector<std::string> words;
    std::vector<std::string> rehearsals;
    std::vector<std::pair<std::string, int>> wedges;
    bool metronome = false;
    nt::NoteValue unit = nt::NoteValue::Quarter;
    int unit_dots = 0;
    double per_minute = 0;
    double sound_tempo = 0;
};

struct RawClef {
    int staff = 1;
    nt::Clef clef = nt::Clef::Treble;
    int octave_change = 0;
};

struct RawTranspose {
    int staff = 0;  ///< 0: every staff.
    int diatonic = 0;
    int chromatic = 0;  ///< Including octave-change.
};

struct RawKey {
    Rational offset;
    int fifths = 0;
    std::string mode = "major";
};

struct RawMeasure {
    std::string number;
    bool implicit = false;
    Rational length;  ///< Furthest position reached.
    std::optional<std::pair<int, int>> time;
    std::vector<RawNote> notes;
    std::vector<RawDirection> directions;
    std::vector<RawKey> keys;
    std::vector<RawClef> clefs;
    std::vector<RawTranspose> transposes;
};

struct PartInfo {
    std::string id, name, abbreviation;
    int program = -1, channel = -1;
    std::map<std::string, int> unpitched;  ///< instrument id -> MIDI note
    bool percussion_channel = false;
};

struct RawPart {
    PartInfo info;
    std::vector<RawMeasure> measures;
    int staves = 1;
};

struct Layout {
    Ticks start = 0;
    Ticks length = 0;
    int numerator = 4, denominator = 4;
};

// --- Element helpers ----------------------------------------------------------------

int int_or(const std::string& s, int fallback)
{
    auto v = detail::parse_int(s);
    if (!v || *v < -1000000 || *v > 1000000) return fallback;
    return static_cast<int>(*v);
}

std::optional<nt::DynamicLevel> level_for(const std::string& name, std::string& text)
{
    nt::DynamicLevel level;
    if (nt::dynamic_from_string(name, level)) return level;
    static const std::pair<const char*, nt::DynamicLevel> kAccents[] = {
        {"sf", nt::DynamicLevel::Forte},       {"sfz", nt::DynamicLevel::Forte},
        {"sffz", nt::DynamicLevel::Fortissimo}, {"sfp", nt::DynamicLevel::Piano},
        {"sfpp", nt::DynamicLevel::Pianissimo}, {"fz", nt::DynamicLevel::Forte},
        {"rf", nt::DynamicLevel::Forte},        {"rfz", nt::DynamicLevel::Forte},
        {"fp", nt::DynamicLevel::Piano},        {"pf", nt::DynamicLevel::Forte},
        {"sfzp", nt::DynamicLevel::Piano},      {"n", nt::DynamicLevel::Pianississimo},
        {"pppp", nt::DynamicLevel::Pianississimo}, {"ppppp", nt::DynamicLevel::Pianississimo},
        {"ffff", nt::DynamicLevel::Fortississimo}, {"fffff", nt::DynamicLevel::Fortississimo},
    };
    for (const auto& k : kAccents)
        if (name == k.first) {
            text = name;
            return k.second;
        }
    return std::nullopt;
}

class Importer {
public:
    ImportResult run(const std::string& document);

private:
    void count(const std::string& what, int n = 1) { counts_[what] += n; }
    void warn(const std::string& w)
    {
        if (std::find(result_.warnings.begin(), result_.warnings.end(), w) == result_.warnings.end())
            result_.warnings.push_back(w);
    }

    bool convert_timewise(xml::Element& root);
    void read_header(const xml::Element& root);
    void read_part(const xml::Element& part_el, RawPart& part);
    void read_attributes(const xml::Element& a, RawMeasure& m, const Rational& pos, long long& divisions,
                         int& staves);
    RawNote read_note(const xml::Element& n, long long divisions);
    void read_notations(const xml::Element& notations, RawNote& note);
    RawDirection read_direction(const xml::Element& d, long long divisions);
    void layout(const std::vector<RawPart>& parts);
    ImportedPart build(const RawPart& part, bool first);

    ImportResult result_;
    std::map<std::string, int> counts_;
    std::vector<Layout> layout_;
    std::map<Ticks, double> tempos_;
    bool divisions_assumed_ = false;
};

// --- Header -----------------------------------------------------------------------

bool Importer::convert_timewise(xml::Element& root)
{
    xml::Element out;
    out.name = "score-partwise";
    out.attributes = root.attributes;
    out.line = root.line;
    std::vector<std::string> order;
    std::map<std::string, xml::Element> parts;
    for (auto& c : root.children) {
        if (c.name != "measure") {
            out.children.push_back(std::move(c));
            continue;
        }
        for (auto& p : c.children) {
            if (p.name != "part") continue;
            const std::string id = p.attribute("id");
            auto it = parts.find(id);
            if (it == parts.end()) {
                xml::Element part;
                part.name = "part";
                part.attributes.emplace_back("id", id);
                part.line = p.line;
                it = parts.emplace(id, std::move(part)).first;
                order.push_back(id);
            }
            xml::Element measure;
            measure.name = "measure";
            measure.attributes = c.attributes;
            measure.line = c.line;
            measure.children = std::move(p.children);
            it->second.children.push_back(std::move(measure));
        }
    }
    for (const auto& id : order) out.children.push_back(std::move(parts[id]));
    root = std::move(out);
    return true;
}

void Importer::read_header(const xml::Element& root)
{
    if (const auto* work = root.child("work")) result_.title = work->child_text("work-title");
    const std::string movement = root.child_text("movement-title");
    if (!movement.empty()) result_.title = movement;
    if (const auto* ident = root.child("identification"))
        for (const auto* c : ident->children_named("creator"))
            if (c->attribute("type") == "composer" && result_.composer.empty()) result_.composer = c->trimmed_text();
}

// --- Pass 1 -------------------------------------------------------------------------

void Importer::read_attributes(const xml::Element& a, RawMeasure& m, const Rational& pos, long long& divisions,
                               int& staves)
{
    for (const auto& c : a.children) {
        if (c.name == "divisions") {
            const auto v = detail::parse_double(c.text);
            if (v && *v > 0 && *v < 1e9 && *v == std::floor(*v)) {
                divisions = static_cast<long long>(*v);
            } else {
                warn("line " + std::to_string(c.line) + ": unusable <divisions> '" + c.trimmed_text() + "' ignored");
            }
        } else if (c.name == "key") {
            if (c.has_attribute("number") && c.attribute("number") != "1") continue;
            if (!c.child("fifths")) {
                count("non-traditional key signatures");
                continue;
            }
            RawKey k;
            k.offset = pos;
            k.fifths = std::clamp(int_or(c.child_text("fifths"), 0), -7, 7);
            k.mode = c.child_text("mode", "major");
            if (k.mode.empty() || k.mode == "none") k.mode = "major";
            m.keys.push_back(k);
        } else if (c.name == "time") {
            if (c.child("senza-misura")) {
                count("senza-misura time signatures");
                continue;
            }
            // "3+2" beats and several <beats>/<beat-type> pairs (composite
            // meters) are summed over the smallest beat type.
            int num = 0, den = 0;
            const auto beats = c.children_named("beats");
            const auto types = c.children_named("beat-type");
            for (std::size_t i = 0; i < beats.size() && i < types.size(); ++i) {
                int b = 0;
                std::string text = beats[i]->trimmed_text();
                std::size_t start = 0;
                while (start <= text.size()) {
                    std::size_t plus = text.find('+', start);
                    if (plus == std::string::npos) plus = text.size();
                    b += int_or(text.substr(start, plus - start), 0);
                    start = plus + 1;
                }
                const int t = int_or(types[i]->trimmed_text(), 0);
                if (b <= 0 || t <= 0) continue;
                if (den == 0) {
                    num = b;
                    den = t;
                } else if (t == den) {
                    num += b;
                } else if (t > den) {
                    num = num * (t / den) + b;
                    den = t;
                } else {
                    num += b * (den / t);
                }
            }
            const TimeSignature sig{num, den};
            if (num > 0 && sig.is_valid()) m.time = std::make_pair(num, den);
            else warn("line " + std::to_string(c.line) + ": unsupported time signature ignored");
        } else if (c.name == "staves") {
            staves = std::clamp(int_or(c.trimmed_text(), 1), 1, 16);
        } else if (c.name == "clef") {
            RawClef rc;
            rc.staff = std::max(1, int_or(c.attribute("number", "1"), 1));
            const std::string sign = c.child_text("sign");
            const int line = int_or(c.child_text("line"), 0);
            rc.octave_change = std::clamp(int_or(c.child_text("clef-octave-change"), 0), -2, 2);
            if (sign == "G") {
                rc.clef = rc.octave_change < 0   ? nt::Clef::TrebleOctaveDown
                          : rc.octave_change > 0 ? nt::Clef::TrebleOctaveUp
                                                 : nt::Clef::Treble;
                if (line != 0 && line != 2) count("unusual G clefs (read as treble)");
            } else if (sign == "F") {
                rc.clef = rc.octave_change < 0 ? nt::Clef::BassOctaveDown : nt::Clef::Bass;
                if (rc.octave_change > 0) {
                    count("bass 8va clefs (read as bass)");
                    rc.octave_change = 0;
                }
                if (line != 0 && line != 4) count("unusual F clefs (read as bass)");
            } else if (sign == "C") {
                rc.clef = line == 4 ? nt::Clef::Tenor : nt::Clef::Alto;
                if (line != 3 && line != 4) count("unusual C clefs (read as alto)");
                if (rc.octave_change != 0) {
                    count("octave C clefs (octave ignored)");
                    rc.octave_change = 0;
                }
            } else if (sign == "percussion") {
                rc.clef = nt::Clef::Percussion;
                rc.octave_change = 0;
            } else if (sign == "TAB") {
                rc.clef = nt::Clef::TrebleOctaveDown;
                rc.octave_change = -1;
                count("tablature staves (read as treble 8vb)");
            } else {
                rc.clef = nt::Clef::Treble;
                rc.octave_change = 0;
            }
            if (std::abs(rc.octave_change) > 1) {
                count("two-octave clefs (read as one octave)");
                rc.octave_change = rc.octave_change < 0 ? -1 : 1;
            }
            m.clefs.push_back(rc);
        } else if (c.name == "transpose") {
            RawTranspose t;
            t.staff = int_or(c.attribute("number", "0"), 0);
            const int oc = int_or(c.child_text("octave-change"), 0);
            t.chromatic = int_or(c.child_text("chromatic"), 0) + 12 * oc;
            t.diatonic = c.child("diatonic") ? int_or(c.child_text("diatonic"), 0) + 7 * oc
                                             : detail::diatonic_for_semitones(t.chromatic);
            if (c.child("double")) t.chromatic -= 12, t.diatonic -= 7;
            m.transposes.push_back(t);
        } else if (c.name == "measure-style") {
            if (c.child("measure-repeat")) count("measure repeats (not expanded)");
            // multiple-rest, slash, beat-repeat: display only.
        }
    }
}

void Importer::read_notations(const xml::Element& notations, RawNote& note)
{
    using A = nt::Articulation;
    for (const auto& c : notations.children) {
        if (c.name == "tied") {
            const std::string t = c.attribute("type");
            if (t == "start" || t == "continue") note.tie_start = true;
        } else if (c.name == "slur") {
            note.slurs.emplace_back(c.attribute("type"), int_or(c.attribute("number", "1"), 1));
        } else if (c.name == "tuplet") {
            RawTuplet t;
            t.start = c.attribute("type") == "start";
            if (c.attribute("type") != "start" && c.attribute("type") != "stop") continue;
            t.number = int_or(c.attribute("number", "1"), 1);
            if (const auto* a = c.child("tuplet-actual")) t.actual = int_or(a->child_text("tuplet-number"), 0);
            if (const auto* n = c.child("tuplet-normal")) t.normal = int_or(n->child_text("tuplet-number"), 0);
            note.tuplets.push_back(t);
        } else if (c.name == "articulations") {
            for (const auto& a : c.children) {
                const std::string& n = a.name;
                if (n == "accent") note.arts |= nt::to_mask(A::Accent);
                else if (n == "strong-accent") note.arts |= nt::to_mask(A::Marcato);
                else if (n == "staccato") note.arts |= nt::to_mask(A::Staccato);
                else if (n == "tenuto") note.arts |= nt::to_mask(A::Tenuto);
                else if (n == "detached-legato") note.arts |= nt::to_mask(A::Portato);
                else if (n == "staccatissimo") note.arts |= nt::to_mask(A::Staccatissimo);
                else if (n == "spiccato") note.arts |= nt::to_mask(A::Spiccato);
                else if (n == "scoop") note.arts |= nt::to_mask(A::Scoop);
                else if (n == "plop") note.arts |= nt::to_mask(A::Plop);
                else if (n == "doit") note.arts |= nt::to_mask(A::Doit);
                else if (n == "falloff") note.arts |= nt::to_mask(A::Falloff);
                else if (n == "other-articulation" && a.trimmed_text() == "legato") note.arts |= nt::to_mask(A::Legato);
                else count("unsupported articulations (" + n + ")");
            }
        } else if (c.name == "ornaments") {
            for (const auto& o : c.children) {
                const std::string& n = o.name;
                if (n == "trill-mark") note.arts |= nt::to_mask(A::Trill);
                else if (n == "mordent" || n == "inverted-mordent") note.arts |= nt::to_mask(A::Mordent);
                else if (n == "turn" || n == "inverted-turn" || n == "delayed-turn" || n == "delayed-inverted-turn")
                    note.arts |= nt::to_mask(A::Turn);
                else if (n == "tremolo") {
                    if (o.attribute("type", "single") == "single") note.arts |= nt::to_mask(A::Tremolo);
                    else count("two-note tremolos");
                } else if (n == "wavy-line" || n == "accidental-mark") {
                    // Extension line of a trill, or its accidental: no meaning of its own here.
                } else {
                    count("unsupported ornaments (" + n + ")");
                }
            }
        } else if (c.name == "technical") {
            for (const auto& t : c.children) {
                const std::string& n = t.name;
                if (n == "up-bow") note.arts |= nt::to_mask(A::UpBow);
                else if (n == "down-bow") note.arts |= nt::to_mask(A::DownBow);
                else if (n == "harmonic") note.arts |= nt::to_mask(A::Harmonic);
                else if (n == "stopped") note.arts |= nt::to_mask(A::Mute);
                else if (n == "snap-pizzicato") note.arts |= nt::to_mask(A::Pizzicato);
                else if (n == "other-technical") {
                    std::string text = t.trimmed_text();
                    for (char& ch : text) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                    if (text == "pizz." || text == "pizz" || text == "pizzicato") note.arts |= nt::to_mask(A::Pizzicato);
                    else count("unsupported technical marks (" + text + ")");
                } else if (n == "fingering" || n == "string" || n == "fret") {
                    count("fingerings and string/fret numbers");
                } else {
                    count("unsupported technical marks (" + n + ")");
                }
            }
        } else if (c.name == "fermata") {
            note.arts |= nt::to_mask(A::Fermata);
        } else if (c.name == "dynamics") {
            for (const auto& d : c.children) {
                std::string text;
                if (d.name == "other-dynamics") {
                    text = d.trimmed_text();
                    nt::DynamicLevel l;
                    if (nt::dynamic_from_string(text, l)) note.dynamics.emplace_back(l, std::string{});
                    else note.dynamics.emplace_back(nt::DynamicLevel::MezzoForte, text);
                } else if (auto l = level_for(d.name, text)) {
                    note.dynamics.emplace_back(*l, text);
                }
            }
        } else if (c.name == "arpeggiate" || c.name == "non-arpeggiate") {
            count("arpeggio signs");
        } else if (c.name == "glissando" || c.name == "slide") {
            if (c.attribute("type") == "start") count("glissandos");
        } else if (c.name == "accidental-mark" || c.name == "other-notation") {
            count("other notations");
        }
    }
}

RawNote Importer::read_note(const xml::Element& n, long long divisions)
{
    RawNote note;
    for (const auto& c : n.children) {
        const std::string& name = c.name;
        if (name == "grace") {
            note.grace = true;
            note.slash = c.attribute("slash") == "yes";
        } else if (name == "cue") {
            note.cue = true;
        } else if (name == "chord") {
            note.chord = true;
        } else if (name == "pitch" || name == "unpitched") {
            const bool unp = name == "unpitched";
            note.unpitched = unp;
            nt::Step step = nt::Step::C;
            const std::string s = c.child_text(unp ? "display-step" : "step", "C");
            if (!nt::step_from_string(s, step)) warn("line " + std::to_string(c.line) + ": unknown step '" + s + "'");
            note.pitch.step = step;
            note.pitch.octave = static_cast<std::int8_t>(std::clamp(int_or(c.child_text(unp ? "display-octave" : "octave", "4"), 4), -1, 9));
            if (!unp) {
                const auto alter = detail::parse_double(c.child_text("alter", "0"));
                double a = alter ? *alter : 0.0;
                if (a != std::round(a)) count("microtonal alterations (rounded)");
                note.pitch.alteration = static_cast<std::int8_t>(std::clamp(static_cast<int>(std::lround(a)), -2, 2));
            }
        } else if (name == "rest") {
            note.rest = true;
            note.measure_rest = c.attribute("measure") == "yes";
        } else if (name == "duration") {
            const auto d = detail::parse_double(c.text);
            long long v = d && *d > 0 && *d < 1e12 ? static_cast<long long>(std::llround(*d)) : 0;
            if (d && *d != std::floor(*d)) count("fractional durations (rounded)");
            note.dur = Rational::of(v, divisions);
        } else if (name == "tie") {
            if (c.attribute("type") == "start") note.tie_start = true;
            if (c.attribute("type") == "stop") note.tie_stop = true;
        } else if (name == "instrument") {
            note.instrument = c.attribute("id");
        } else if (name == "voice") {
            note.voice = c.trimmed_text();
        } else if (name == "type") {
            note.has_type = true;
            note.type = detail::type_from_name(c.trimmed_text());
            if (!note.type) count("note values shorter than a 64th or longer than a breve (derived from duration)");
        } else if (name == "dot") {
            ++note.dots;
        } else if (name == "time-modification") {
            note.tm_actual = std::max(1, int_or(c.child_text("actual-notes"), 1));
            note.tm_normal = std::max(1, int_or(c.child_text("normal-notes"), 1));
        } else if (name == "staff") {
            note.staff = std::clamp(int_or(c.trimmed_text(), 1), 1, 16);
        } else if (name == "notations") {
            read_notations(c, note);
        } else if (name == "lyric") {
            count("lyrics");
        }
    }
    if (n.attribute("print-object") == "no" && note.rest) note.invisible = true;
    if (note.voice.empty()) note.voice = "1";
    return note;
}

RawDirection Importer::read_direction(const xml::Element& d, long long divisions)
{
    RawDirection dir;
    for (const auto& c : d.children) {
        if (c.name == "direction-type") {
            for (const auto& t : c.children) {
                if (t.name == "dynamics") {
                    for (const auto& x : t.children) {
                        std::string text;
                        if (x.name == "other-dynamics") {
                            text = x.trimmed_text();
                            nt::DynamicLevel l;
                            if (nt::dynamic_from_string(text, l)) dir.dynamics.emplace_back(l, std::string{});
                            else dir.dynamics.emplace_back(nt::DynamicLevel::MezzoForte, text);
                        } else if (auto l = level_for(x.name, text)) {
                            dir.dynamics.emplace_back(*l, text);
                        } else {
                            count("unsupported dynamics (" + x.name + ")");
                        }
                    }
                } else if (t.name == "wedge") {
                    dir.wedges.emplace_back(t.attribute("type"), int_or(t.attribute("number", "1"), 1));
                } else if (t.name == "words") {
                    const std::string w = t.trimmed_text();
                    if (!w.empty()) dir.words.push_back(w);
                } else if (t.name == "rehearsal") {
                    const std::string w = t.trimmed_text();
                    if (!w.empty()) dir.rehearsals.push_back(w);
                } else if (t.name == "metronome") {
                    const auto units = t.children_named("beat-unit");
                    const auto pm = detail::parse_double(t.child_text("per-minute"));
                    if (units.size() == 1 && pm && *pm > 0) {
                        dir.metronome = true;
                        dir.per_minute = *pm;
                        dir.unit = detail::type_from_name(units[0]->trimmed_text()).value_or(nt::NoteValue::Quarter);
                        dir.unit_dots = static_cast<int>(t.children_named("beat-unit-dot").size());
                    } else {
                        count("metric modulations and non-numeric metronome marks");
                    }
                } else if (t.name == "pedal" || t.name == "octave-shift" || t.name == "segno" || t.name == "coda" ||
                           t.name == "bracket" || t.name == "dashes" || t.name == "eyeglasses" ||
                           t.name == "damp" || t.name == "accordion-registration" || t.name == "harp-pedals" ||
                           t.name == "scordatura" || t.name == "string-mute" || t.name == "principal-voice" ||
                           t.name == "percussion" || t.name == "image" || t.name == "symbol" ||
                           t.name == "staff-divide" || t.name == "swing" || t.name == "other-direction") {
                    if (t.name == "dashes" || t.name == "bracket") continue;  // extension lines
                    count(t.name + " directions");
                }
            }
        } else if (c.name == "offset") {
            const auto v = detail::parse_double(c.text);
            if (v) dir.offset = Rational::of(static_cast<long long>(std::llround(*v)), divisions);
        } else if (c.name == "staff") {
            dir.staff = std::clamp(int_or(c.trimmed_text(), 1), 1, 16);
        } else if (c.name == "voice") {
            dir.voice = c.trimmed_text();
        } else if (c.name == "sound") {
            const auto t = detail::parse_double(c.attribute("tempo"));
            if (t && *t > 0) dir.sound_tempo = *t;
        }
    }
    return dir;
}

void Importer::read_part(const xml::Element& part_el, RawPart& part)
{
    long long divisions = 1;
    bool have_divisions = false;
    int staves = 1;
    for (const auto& m_el : part_el.children) {
        if (m_el.name != "measure") continue;
        RawMeasure m;
        m.number = m_el.attribute("number");
        m.implicit = m_el.attribute("implicit") == "yes";
        Rational pos;
        Rational last_offset;
        bool backup_clamped = false;
        for (const auto& c : m_el.children) {
            const std::string& name = c.name;
            if (name == "attributes") {
                read_attributes(c, m, pos, divisions, staves);
                if (c.child("divisions")) have_divisions = true;
            } else if (name == "note") {
                if (!have_divisions && !divisions_assumed_) {
                    divisions_assumed_ = true;
                    warn("a note comes before any <divisions>; 1 division per quarter assumed");
                }
                RawNote note = read_note(c, divisions);
                if (note.grace) note.dur = Rational{};
                if (note.chord) {
                    note.offset = last_offset;
                } else {
                    note.offset = pos;
                    last_offset = pos;
                    pos = pos + note.dur;
                }
                if (pos > m.length) m.length = pos;
                const Rational end = note.offset + note.dur;
                if (end > m.length) m.length = end;
                staves = std::max(staves, note.staff);
                m.notes.push_back(std::move(note));
            } else if (name == "backup" || name == "forward") {
                const auto d = detail::parse_double(c.child_text("duration"));
                const Rational r = Rational::of(d && *d > 0 ? static_cast<long long>(std::llround(*d)) : 0, divisions);
                if (name == "backup") {
                    pos = pos - r;
                    if (pos.negative()) {
                        pos = Rational{};
                        backup_clamped = true;
                    }
                } else {
                    pos = pos + r;
                    if (pos > m.length) m.length = pos;
                }
            } else if (name == "direction") {
                RawDirection d = read_direction(c, divisions);
                d.offset = pos + d.offset;
                if (d.offset.negative()) d.offset = Rational{};
                staves = std::max(staves, d.staff);
                m.directions.push_back(std::move(d));
            } else if (name == "sound") {
                const auto t = detail::parse_double(c.attribute("tempo"));
                if (t && *t > 0) {
                    RawDirection d;
                    d.offset = pos;
                    d.sound_tempo = *t;
                    m.directions.push_back(std::move(d));
                }
            } else if (name == "barline") {
                if (c.child("repeat")) count("repeat barlines (music not unfolded)");
                if (c.child("ending")) count("volta endings");
            } else if (name == "harmony") {
                count("chord symbols");
            } else if (name == "figured-bass") {
                count("figured bass figures");
            } else if (name == "print" || name == "bookmark" || name == "link" || name == "grouping" ||
                       name == "listening") {
                // Layout and analysis: nothing to import.
            } else {
                count("unknown <" + name + "> elements");
            }
        }
        if (backup_clamped) count("backups before the start of a measure (clamped)");
        part.measures.push_back(std::move(m));
    }
    part.staves = staves;
}

// --- Pass 2 -------------------------------------------------------------------------

std::optional<std::pair<int, int>> signature_for(Ticks length, int preferred_den)
{
    for (int den = std::max(1, preferred_den); den <= 64; den *= 2) {
        const Ticks beat = kTicksPerWhole / den;
        if (length % beat == 0 && length / beat > 0 && length / beat <= 64)
            return std::make_pair(static_cast<int>(length / beat), den);
    }
    return std::nullopt;
}

void Importer::layout(const std::vector<RawPart>& parts)
{
    std::size_t count_measures = 0;
    for (const auto& p : parts) count_measures = std::max(count_measures, p.measures.size());
    std::pair<int, int> sig{4, 4};
    bool have_time = false;
    Ticks start = 0;
    int prev_num = 0, prev_den = 0;
    for (std::size_t i = 0; i < count_measures; ++i) {
        Ticks content = 0;
        bool implicit = false;
        bool number_zero = false;
        for (const auto& p : parts) {
            if (i >= p.measures.size()) continue;
            const RawMeasure& m = p.measures[i];
            content = std::max(content, m.length.ticks());
            implicit = implicit || m.implicit;
            number_zero = number_zero || m.number == "0";
        }
        for (const auto& p : parts)
            if (i < p.measures.size() && p.measures[i].time) {
                sig = *p.measures[i].time;
                have_time = true;
                break;
            }
        Layout l;
        l.start = start;
        const Ticks nominal = sig.first * kTicksPerWhole / sig.second;
        l.length = nominal;
        l.numerator = sig.first;
        l.denominator = sig.second;
        if (content > 0 && content < nominal && (implicit || (i == 0 && number_zero))) {
            if (auto s = signature_for(content, sig.second)) {
                l.length = content;
                l.numerator = s->first;
                l.denominator = s->second;
                if (i == 0) {
                    result_.has_pickup = true;
                    result_.pickup_ticks = content;
                } else {
                    count("short implicit measures (kept as bars of their own length)");
                }
            } else {
                count("pickups of a length no time signature can hold (padded to a full bar)");
            }
        } else if (content > nominal) {
            if (auto s = signature_for(content, sig.second)) {
                l.length = content;
                l.numerator = s->first;
                l.denominator = s->second;
            }
            count("overfull measures (lengthened)");
        }
        if (!have_time && i == 0) warn("no time signature in the file; 4/4 assumed");
        if (l.numerator != prev_num || l.denominator != prev_den) {
            result_.time_signatures.push_back({l.start, static_cast<int>(i) + 1, l.numerator, l.denominator});
            prev_num = l.numerator;
            prev_den = l.denominator;
        }
        layout_.push_back(l);
        start += l.length;
    }
    if (layout_.empty()) {
        layout_.push_back(Layout{0, kTicksPerWhole, 4, 4});
        result_.time_signatures.push_back({0, 1, 4, 4});
    }
    result_.measure_count = static_cast<int>(count_measures);
}

// --- Pass 3 -------------------------------------------------------------------------

bool minor_mode(const std::string& mode)
{
    return mode == "minor" || mode == "aeolian" || mode == "dorian" || mode == "phrygian" || mode == "locrian";
}

ImportedPart Importer::build(const RawPart& part, bool first)
{
    ImportedPart out;
    out.id = part.info.id;
    out.name = part.info.name.empty() ? part.info.id : part.info.name;
    out.abbreviation = part.info.abbreviation;
    out.midi_program = part.info.program;
    out.midi_channel = part.info.channel;
    out.is_percussion = part.info.percussion_channel;

    const int staves = std::max(1, part.staves);

    // Staves: the first clef and transposition each staff is given.
    std::vector<nt::Staff> staff_defs(static_cast<std::size_t>(staves));
    std::vector<int> clef_octave(static_cast<std::size_t>(staves), 0);
    std::vector<bool> clef_seen(static_cast<std::size_t>(staves), false);
    std::vector<bool> transpose_seen(static_cast<std::size_t>(staves), false);
    std::vector<RawTranspose> transposes(static_cast<std::size_t>(staves));
    bool clef_changed = false, transpose_changed = false;
    for (const auto& m : part.measures) {
        for (const auto& c : m.clefs) {
            if (c.staff > staves) continue;
            const auto s = static_cast<std::size_t>(c.staff - 1);
            if (!clef_seen[s]) {
                clef_seen[s] = true;
                staff_defs[s].clef = c.clef;
                staff_defs[s].octave_shift = static_cast<std::int8_t>(c.octave_change);
                clef_octave[s] = c.octave_change;
            } else if (c.clef != staff_defs[s].clef) {
                clef_changed = true;
            }
        }
        for (const auto& t : m.transposes) {
            for (int s = 1; s <= staves; ++s) {
                if (t.staff != 0 && t.staff != s) continue;
                const auto i = static_cast<std::size_t>(s - 1);
                if (!transpose_seen[i]) {
                    transpose_seen[i] = true;
                    transposes[i] = t;
                } else if (t.chromatic != transposes[i].chromatic) {
                    transpose_changed = true;
                }
            }
        }
    }
    if (clef_changed) count("clef changes (each staff keeps its first clef)");
    if (transpose_changed) count("transposition changes (the first is kept)");
    for (int s = 0; s < staves; ++s) {
        auto& sd = staff_defs[static_cast<std::size_t>(s)];
        sd.transposition_semitones =
            static_cast<std::int8_t>(std::clamp(transposes[static_cast<std::size_t>(s)].chromatic, -48, 48));
        sd.name = staves == 1 ? out.name
                  : staves == 2 ? out.name + (s == 0 ? " (upper)" : " (lower)")
                                : out.name + " (" + std::to_string(s + 1) + ")";
        if (sd.clef == nt::Clef::Percussion) out.is_percussion = true;
    }
    out.score.reset_staves(staff_defs);

    // Meter.
    for (const auto& t : result_.time_signatures)
        out.score.set_time_signature(t.tick, music::TimeSignature{static_cast<std::uint16_t>(t.numerator),
                                                                  static_cast<std::uint16_t>(t.denominator)});

    // Keys, at concert pitch.
    const RawTranspose& t0 = transposes[0];
    std::vector<ImportedKey> keys;
    for (std::size_t i = 0; i < part.measures.size() && i < layout_.size(); ++i)
        for (const auto& k : part.measures[i].keys) {
            ImportedKey ik;
            ik.tick = layout_[i].start + k.offset.ticks();
            ik.fifths = detail::normalise_fifths(k.fifths + detail::fifths_delta(t0.diatonic, t0.chromatic));
            ik.mode = k.mode;
            if (!keys.empty() && keys.back().tick == ik.tick) keys.back() = ik;
            else if (keys.empty() || keys.back().fifths != ik.fifths || keys.back().mode != ik.mode) keys.push_back(ik);
        }
    if (keys.empty() || keys.front().tick != 0) keys.insert(keys.begin(), ImportedKey{});
    for (const auto& k : keys) {
        nt::KeySignature ks;
        ks.fifths = static_cast<std::int8_t>(k.fifths);
        ks.is_minor = minor_mode(k.mode);
        out.score.set_key_signature(k.tick, ks);
    }
    if (first) result_.keys = keys;

    // Voices: per staff, the file's voice names in numeric order -> 0..3.
    std::vector<std::vector<std::string>> voice_names(static_cast<std::size_t>(staves));
    for (const auto& m : part.measures)
        for (const auto& n : m.notes) {
            if (n.cue) continue;
            auto& v = voice_names[static_cast<std::size_t>(std::min(n.staff, staves) - 1)];
            if (std::find(v.begin(), v.end(), n.voice) == v.end()) v.push_back(n.voice);
        }
    for (auto& v : voice_names) {
        std::stable_sort(v.begin(), v.end(), [](const std::string& a, const std::string& b) {
            const auto x = detail::parse_int(a), y = detail::parse_int(b);
            if (x && y) return *x < *y;
            if (x != y) return x.has_value();
            return a < b;
        });
        if (v.size() > 4) count("voices beyond the fourth on a staff (merged into voice 4)");
    }
    auto voice_index = [&](int staff, const std::string& name) -> std::uint8_t {
        const auto& v = voice_names[static_cast<std::size_t>(staff)];
        auto it = std::find(v.begin(), v.end(), name);
        const auto i = it == v.end() ? 0 : static_cast<int>(it - v.begin());
        return static_cast<std::uint8_t>(std::min(i, 3));
    };

    // Notes.
    struct OpenSlur {
        Ticks tick;
        int staff;
        int voice;
    };
    struct OpenWedge {
        Ticks tick;
        nt::HairpinKind kind;
        int staff;
    };
    std::map<int, OpenSlur> slurs;
    std::map<int, OpenWedge> wedges;
    std::map<std::pair<int, std::string>, std::vector<std::pair<int, nt::Tuplet>>> tuplet_stacks;
    std::map<std::pair<int, int>, int> grace_counter;
    std::vector<std::pair<std::size_t, nt::ScoreNote>> pending;
    int cue_notes = 0;
    bool untyped = false, approximated = false;

    for (std::size_t mi = 0; mi < part.measures.size() && mi < layout_.size(); ++mi) {
        const RawMeasure& m = part.measures[mi];
        const Layout& lay = layout_[mi];
        int last_grace_order = 0;
        for (const RawNote& rn : m.notes) {
            const int staff = std::min(rn.staff, staves) - 1;
            const auto s = static_cast<std::size_t>(staff);
            if (rn.cue) {
                ++cue_notes;
                continue;
            }
            const Ticks tick = lay.start + rn.offset.ticks();
            const std::uint8_t voice = voice_index(staff, rn.voice);

            // Tuplet brackets opening on this note.
            auto& stack = tuplet_stacks[{staff, rn.voice}];
            for (const auto& t : rn.tuplets) {
                if (!t.start) continue;
                nt::Tuplet ratio{static_cast<std::uint16_t>(rn.tm_actual), static_cast<std::uint16_t>(rn.tm_normal)};
                if (t.actual > 0 && t.normal > 0) {
                    ratio = {static_cast<std::uint16_t>(t.actual), static_cast<std::uint16_t>(t.normal)};
                } else if (!stack.empty()) {
                    const nt::Tuplet& o = stack.back().second;
                    if (rn.tm_actual % o.actual == 0 && rn.tm_normal % o.normal == 0)
                        ratio = {static_cast<std::uint16_t>(rn.tm_actual / o.actual),
                                 static_cast<std::uint16_t>(rn.tm_normal / o.normal)};
                }
                stack.emplace_back(t.number, ratio);
            }

            nt::ScoreNote note;
            note.start_tick = tick;
            note.voice = voice;
            note.articulations = rn.arts;
            note.grace = rn.grace;
            note.grace_slashed = rn.slash;
            note.is_rest = rn.rest;
            if (rn.rest) {
                note.pitch = {};
            } else if (rn.unpitched) {
                auto it = part.info.unpitched.find(rn.instrument);
                if (it == part.info.unpitched.end() && part.info.unpitched.size() == 1) it = part.info.unpitched.begin();
                note.pitch = it != part.info.unpitched.end() ? nt::spell_midi_note(it->second, nt::KeySignature{})
                                                             : rn.pitch;
                out.is_percussion = true;
            } else {
                note.pitch = rn.pitch;
                note.pitch.octave = static_cast<std::int8_t>(note.pitch.octave - clef_octave[s]);
            }

            // Written value.
            nt::Duration dur;
            dur.value = rn.type.value_or(nt::NoteValue::Quarter);
            dur.dots = static_cast<std::uint8_t>(std::min(rn.dots, 3));
            if (rn.tm_actual != rn.tm_normal) {
                long long a = rn.tm_actual, n = rn.tm_normal;
                bool nested = false;
                if (stack.size() >= 2) {
                    const nt::Tuplet& r1 = stack[stack.size() - 2].second;
                    const nt::Tuplet& r2 = stack.back().second;
                    if (static_cast<long long>(r1.actual) * r2.actual == a &&
                        static_cast<long long>(r1.normal) * r2.normal == n) {
                        dur.outer = r1;
                        dur.tuplet = r2;
                        nested = true;
                    }
                }
                if (!nested) {
                    const long long g = detail::gcd_ll(a, n);
                    if (a > 32 || n > 32) a /= g, n /= g;
                    dur.tuplet = {static_cast<std::uint16_t>(std::min(a, 32LL)),
                                  static_cast<std::uint16_t>(std::min(n, 32LL))};
                }
            }

            std::vector<nt::Duration> pieces;
            const Ticks ticks = rn.grace ? 0 : rn.dur.ticks();
            if (rn.grace) {
                if (!rn.type) dur.value = nt::NoteValue::Eighth;
                dur.tuplet = {};
                dur.outer = {};
                if (!dur.is_valid()) dur = nt::eighth();
                pieces.push_back(dur);
            } else if (ticks <= 0) {
                if (!rn.measure_rest) warn("notes without a duration were skipped");
            } else if (rn.type && dur.is_valid() && dur.ticks() == ticks) {
                pieces.push_back(dur);
            } else {
                if (!rn.type) untyped = true;
                // Find a single value with the same tuplet, else a tie chain.
                bool found = false;
                for (int v = 0; v <= 7 && !found; ++v)
                    for (int d = 0; d <= 3 && !found; ++d) {
                        nt::Duration c = dur;
                        c.value = static_cast<nt::NoteValue>(v);
                        c.dots = static_cast<std::uint8_t>(d);
                        if (c.is_valid() && c.ticks() == ticks) {
                            pieces.push_back(c);
                            found = true;
                        }
                    }
                if (!found) {
                    nt::Duration buf[16];
                    std::size_t k = 0;
                    const long long num = static_cast<long long>(dur.tuplet.actual) * dur.outer.actual;
                    const long long den = static_cast<long long>(dur.tuplet.normal) * dur.outer.normal;
                    if (dur.tuplet.is_tuplet() && (ticks * num) % den == 0) {
                        k = nt::decompose_ticks(ticks * num / den, buf, 16);
                        for (std::size_t i = 0; i < k; ++i) {
                            buf[i].tuplet = dur.tuplet;
                            buf[i].outer = dur.outer;
                            if (!buf[i].is_valid()) k = 0;
                        }
                    }
                    if (k == 0) k = nt::decompose_ticks(ticks, buf, 16);
                    if (k == 0) {
                        // Off any grid the model can hold: round to a 64th triplet.
                        const Ticks grid = nt::kTicksPerWhole / 96;
                        const Ticks rounded = std::max<Ticks>(grid, (ticks + grid / 2) / grid * grid);
                        k = nt::decompose_ticks(rounded, buf, 16);
                        approximated = true;
                    }
                    pieces.assign(buf, buf + k);
                }
            }
            if (pieces.empty()) {
                for (const auto& t : rn.tuplets)
                    if (!t.start && !stack.empty()) stack.pop_back();
                continue;
            }

            // Grace order within the voice.
            if (rn.grace) {
                auto& counter = grace_counter[{staff, voice}];
                if (rn.chord) {
                    note.grace_order = static_cast<std::uint8_t>(last_grace_order);
                } else {
                    last_grace_order = std::min(counter, 255);
                    note.grace_order = static_cast<std::uint8_t>(last_grace_order);
                    ++counter;
                }
            } else if (!rn.chord) {
                grace_counter[{staff, voice}] = 0;
            }

            const bool skip = rn.rest && (rn.measure_rest || rn.invisible);
            if (!skip) {
                Ticks at = tick;
                for (std::size_t i = 0; i < pieces.size(); ++i) {
                    nt::ScoreNote piece = note;
                    piece.start_tick = at;
                    piece.duration = pieces[i];
                    if (i > 0) piece.articulations = 0;
                    const bool last = i + 1 == pieces.size();
                    piece.tied_to_next = !rn.rest && !rn.grace && (!last || rn.tie_start);
                    pending.emplace_back(s, piece);
                    at += pieces[i].ticks();
                }
            }

            // Slurs.
            const Ticks note_end = tick + ticks;
            for (const auto& [type, number] : rn.slurs) {
                if (type == "start") {
                    slurs[number] = OpenSlur{tick, staff, voice};
                } else if (type == "stop") {
                    auto it = slurs.find(number);
                    if (it == slurs.end()) {
                        count("slur ends without a start");
                        continue;
                    }
                    nt::Slur sl;
                    sl.start_tick = it->second.tick;
                    sl.end_tick = std::max(note_end, tick);
                    sl.staff = static_cast<std::uint8_t>(it->second.staff);
                    sl.voice = static_cast<std::uint8_t>(it->second.voice);
                    if (sl.is_valid()) out.score.add_slur(sl);
                    slurs.erase(it);
                }
            }
            for (const auto& [level, text] : rn.dynamics) {
                nt::DynamicMarking d;
                d.tick = tick;
                d.level = level;
                d.text = text;
                d.staff = static_cast<std::uint8_t>(staff);
                out.score.add_dynamic(d);
            }
            // Tuplet brackets closing on this note.
            for (const auto& t : rn.tuplets) {
                if (t.start) continue;
                for (auto it = stack.rbegin(); it != stack.rend(); ++it)
                    if (it->first == t.number) {
                        stack.erase(std::next(it).base());
                        break;
                    }
            }
        }

        // Directions.
        for (const RawDirection& d : m.directions) {
            const Ticks tick = lay.start + d.offset.ticks();
            const int staff = std::min(d.staff, staves) - 1;
            for (const auto& [level, text] : d.dynamics) {
                nt::DynamicMarking dm;
                dm.tick = tick;
                dm.level = level;
                dm.text = text;
                dm.staff = static_cast<std::uint8_t>(staff);
                out.score.add_dynamic(dm);
            }
            for (const auto& [type, number] : d.wedges) {
                if (type == "crescendo" || type == "diminuendo") {
                    wedges[number] = OpenWedge{
                        tick, type == "crescendo" ? nt::HairpinKind::Crescendo : nt::HairpinKind::Diminuendo, staff};
                } else if (type == "stop") {
                    auto it = wedges.find(number);
                    if (it == wedges.end()) continue;
                    nt::Hairpin h;
                    h.start_tick = it->second.tick;
                    h.end_tick = tick;
                    h.kind = it->second.kind;
                    h.staff = static_cast<std::uint8_t>(it->second.staff);
                    if (h.is_valid()) out.score.add_hairpin(h);
                    else count("zero-length hairpins");
                    wedges.erase(it);
                }
            }
            std::string words;
            for (const auto& w : d.words) words += (words.empty() ? "" : " ") + w;
            const bool tempo = d.metronome || d.sound_tempo > 0;
            if (tempo) {
                double quarter_bpm = d.sound_tempo;
                if (quarter_bpm <= 0) {
                    nt::Duration unit{d.unit, static_cast<std::uint8_t>(std::min(d.unit_dots, 3)), {}};
                    quarter_bpm = d.per_minute * static_cast<double>(unit.ticks()) / kTicksPerQuarter;
                }
                if (quarter_bpm > 0 && quarter_bpm <= 1000 && tempos_.find(tick) == tempos_.end())
                    tempos_[tick] = quarter_bpm;
                if (d.metronome || !words.empty()) {
                    nt::TempoMarking tm;
                    tm.tick = tick;
                    tm.text = words;
                    if (d.metronome) {
                        tm.beats_per_minute = d.per_minute;
                        tm.beat_unit = d.unit;
                        tm.beat_unit_dots = static_cast<std::uint8_t>(std::min(d.unit_dots, 3));
                    } else {
                        tm.beats_per_minute = quarter_bpm;
                    }
                    if (tm.is_valid()) out.score.add_tempo(tm);
                }
            } else if (!words.empty()) {
                nt::TextMarking tm;
                tm.tick = tick;
                tm.text = words;
                if (!d.voice.empty()) {
                    const auto& v = voice_names[static_cast<std::size_t>(staff)];
                    auto it = std::find(v.begin(), v.end(), d.voice);
                    if (it != v.end()) tm.voice = static_cast<std::uint8_t>(std::min<long>(it - v.begin(), 3));
                }
                out.score.add_text(tm);
            }
            for (const auto& r : d.rehearsals) {
                nt::TextMarking tm;
                tm.tick = tick;
                tm.text = r;
                out.score.add_text(tm);
            }
        }
    }
    if (!slurs.empty()) count("slurs without an end", static_cast<int>(slurs.size()));
    if (!wedges.empty()) count("hairpins without an end", static_cast<int>(wedges.size()));
    if (cue_notes > 0) count("cue notes (skipped)", cue_notes);
    if (untyped) warn("notes without <type> were given values from their durations");
    if (approximated) warn("durations off the score's grid were rounded to the nearest 64th triplet");

    // Chords: notes sharing a staff, voice and start are one chord.
    std::map<std::tuple<std::size_t, int, Ticks, bool, int>, int> members;
    for (const auto& [s, n] : pending)
        if (!n.is_rest) ++members[{s, n.voice, n.start_tick, n.grace, n.grace_order}];
    int rejected = 0;
    for (auto& [s, n] : pending) {
        if (!n.is_rest) n.in_chord = members[{s, n.voice, n.start_tick, n.grace, n.grace_order}] > 1;
        if (out.score.insert_note(s, n) == nt::kInvalidEventId) ++rejected;
    }
    if (rejected > 0) count("notes the score model rejected", rejected);
    return out;
}

// --- Driver -------------------------------------------------------------------------

ImportResult Importer::run(const std::string& document)
{
    auto parsed = xml::parse(document);
    if (!parsed.ok) {
        result_.error = parsed.error;
        return result_;
    }
    xml::Element& root = parsed.root;
    if (root.name == "score-timewise") {
        convert_timewise(root);
        warn("score-timewise converted to score-partwise");
    } else if (root.name != "score-partwise") {
        result_.error = "line " + std::to_string(root.line) + ": not a MusicXML score (the root element is <" +
                        root.name + ">" + (root.name == "opus" ? "; open one of the opus's scores instead" : "") + ")";
        return result_;
    }
    read_header(root);

    // Part list.
    std::vector<PartInfo> infos;
    if (const auto* list = root.child("part-list")) {
        for (const auto& sp : list->children) {
            if (sp.name != "score-part") continue;
            PartInfo info;
            info.id = sp.attribute("id");
            info.name = sp.child_text("part-name");
            info.abbreviation = sp.child_text("part-abbreviation");
            for (const auto* mi : sp.children_named("midi-instrument")) {
                const int channel = int_or(mi->child_text("midi-channel"), 0);
                const int program = int_or(mi->child_text("midi-program"), 0);
                if (info.channel < 0 && channel >= 1 && channel <= 16) info.channel = channel - 1;
                if (info.program < 0 && program >= 1 && program <= 128) info.program = program - 1;
                if (channel == 10) info.percussion_channel = true;
                const int unpitched = int_or(mi->child_text("midi-unpitched"), 0);
                if (unpitched >= 1 && unpitched <= 128) info.unpitched[mi->attribute("id")] = unpitched - 1;
            }
            infos.push_back(info);
        }
    }

    std::vector<RawPart> parts;
    for (const auto& c : root.children) {
        if (c.name != "part") continue;
        RawPart rp;
        const std::string id = c.attribute("id");
        auto it = std::find_if(infos.begin(), infos.end(), [&](const PartInfo& i) { return i.id == id; });
        if (it != infos.end()) rp.info = *it;
        else rp.info.id = id;
        read_part(c, rp);
        parts.push_back(std::move(rp));
    }
    if (parts.empty()) {
        result_.error = "line " + std::to_string(root.line) + ": the score has no parts";
        return result_;
    }

    layout(parts);
    for (std::size_t i = 0; i < parts.size(); ++i) result_.parts.push_back(build(parts[i], i == 0));
    for (const auto& [tick, bpm] : tempos_) result_.tempos.push_back({tick, bpm});

    // "12 lyrics ignored"; a label that says what was done instead stands alone.
    for (const auto& [what, n] : counts_)
        result_.warnings.push_back(std::to_string(n) + " " + what +
                                   (what.find('(') == std::string::npos ? " ignored" : ""));
    result_.ok = true;
    return result_;
}

}  // namespace

ImportResult import_musicxml(const std::string& document)
{
    try {
        return Importer().run(document);
    } catch (const std::exception& e) {
        ImportResult r;
        r.error = std::string("import failed: ") + e.what();
        return r;
    } catch (...) {
        ImportResult r;
        r.error = "import failed";
        return r;
    }
}

}  // namespace vcc::interchange
