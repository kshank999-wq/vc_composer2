// MusicXML export: project -> score-partwise 4.0.
//
// Per part the work is in three passes. First every note of the track's
// score is grouped into chords per staff and voice and cut at barlines
// (with ties), giving written events per measure. Then each measure's
// events are laid into MusicXML voices, tuplet brackets, slurs and
// accidentals are worked out, and the smallest <divisions> that expresses
// every duration exactly is chosen. Last the measures are written.
#include "vcc/interchange/MusicXml.h"

#include <algorithm>
#include <cctype>
#include <climits>
#include <map>
#include <set>

#include "MusicXmlCommon.h"
#include "vcc/interchange/Xml.h"

namespace vcc::interchange {

namespace nt = vcc::notation;
using detail::format_number;

namespace {

// --- Tables -------------------------------------------------------------------

int clef_octave(nt::Clef c) noexcept
{
    switch (c) {
        case nt::Clef::TrebleOctaveDown:
        case nt::Clef::BassOctaveDown: return -1;
        case nt::Clef::TrebleOctaveUp: return 1;
        default: return 0;
    }
}

void write_clef(xml::Writer& w, nt::Clef c, int number)
{
    xml::Attributes attrs;
    if (number > 0) attrs.emplace_back("number", std::to_string(number));
    w.open("clef", attrs);
    switch (c) {
        case nt::Clef::Treble:
        case nt::Clef::TrebleOctaveDown:
        case nt::Clef::TrebleOctaveUp:
            w.leaf("sign", "G");
            w.leaf("line", "2");
            break;
        case nt::Clef::Bass:
        case nt::Clef::BassOctaveDown:
            w.leaf("sign", "F");
            w.leaf("line", "4");
            break;
        case nt::Clef::Alto:
            w.leaf("sign", "C");
            w.leaf("line", "3");
            break;
        case nt::Clef::Tenor:
            w.leaf("sign", "C");
            w.leaf("line", "4");
            break;
        case nt::Clef::Percussion: w.leaf("sign", "percussion"); break;
    }
    if (clef_octave(c) != 0) w.leaf("clef-octave-change", std::to_string(clef_octave(c)));
    w.close();
}

const char* accidental_name(int alteration) noexcept
{
    switch (alteration) {
        case -2: return "flat-flat";
        case -1: return "flat";
        case 1: return "sharp";
        case 2: return "double-sharp";
        default: return "natural";
    }
}

/// Where an unpitched General MIDI drum sits on a five-line percussion
/// staff, roughly as drum-set notation places it.
nt::Pitch drum_display(int midi) noexcept
{
    auto at = [](nt::Step s, int octave) { return nt::Pitch{s, static_cast<std::int8_t>(octave), 0}; };
    switch (midi) {
        case 35:
        case 36: return at(nt::Step::F, 4);
        case 37:
        case 38:
        case 40: return at(nt::Step::C, 5);
        case 41:
        case 43: return at(nt::Step::A, 4);
        case 45:
        case 47: return at(nt::Step::D, 5);
        case 48:
        case 50: return at(nt::Step::E, 5);
        case 42:
        case 44:
        case 46: return at(nt::Step::G, 5);
        case 49:
        case 57: return at(nt::Step::A, 5);
        case 51:
        case 59: return at(nt::Step::F, 5);
        default: return at(nt::Step::E, 4);
    }
}

std::string drum_name(int midi)
{
    switch (midi) {
        case 35: return "Acoustic Bass Drum";
        case 36: return "Bass Drum";
        case 37: return "Side Stick";
        case 38: return "Snare";
        case 39: return "Hand Clap";
        case 40: return "Electric Snare";
        case 41: return "Low Floor Tom";
        case 42: return "Closed Hi-Hat";
        case 43: return "High Floor Tom";
        case 44: return "Pedal Hi-Hat";
        case 45: return "Low Tom";
        case 46: return "Open Hi-Hat";
        case 47: return "Low-Mid Tom";
        case 48: return "Hi-Mid Tom";
        case 49: return "Crash Cymbal";
        case 50: return "High Tom";
        case 51: return "Ride Cymbal";
        default: return "Percussion " + std::to_string(midi);
    }
}

// --- Event model --------------------------------------------------------------

struct MeasureInfo {
    int bar = 1;
    Ticks start = 0;
    Ticks length = 0;
    TimeSignature sig;
};

/// One written note or rest: a ScoreNote, or the part of one that falls in
/// a measure.
struct Piece {
    const nt::ScoreNote* src = nullptr;
    nt::Pitch pitch;       ///< As written in the file (transposed / octave-clef adjusted).
    int midi = 0;          ///< Sounding MIDI note, for percussion.
    bool tie_start = false;
    bool tie_stop = false;
    bool first = true;     ///< Carries the note's articulations.
    int accidental = INT_MIN;  ///< Alteration to print, INT_MIN for none.
};

struct TupletMark {
    int number = 1;
    bool start = true;
    nt::Tuplet ratio;
    nt::NoteValue unit = nt::NoteValue::Eighth;
};

struct Event {
    Ticks offset = 0;  ///< From the measure start.
    Ticks ticks = 0;   ///< Sounding length (0 for a grace note).
    nt::Duration dur;
    bool exact = true;  ///< `dur` matches `ticks`, so <type> can be written.
    bool rest = false;
    bool measure_rest = false;
    bool grace = false;
    bool slash = true;
    int grace_order = 0;
    std::vector<Piece> notes;
    std::vector<TupletMark> tuplets;
    std::vector<int> slur_starts;
    std::vector<int> slur_stops;
};

struct Lane {
    int staff = 0;
    int voice = 0;      ///< Model voice; -1 for an overflow lane.
    int xml_voice = 1;
    std::vector<Event> events;
    Ticks end = 0;
};

/// A chord (or single note or rest) of the model before barline splitting.
struct Chord {
    std::vector<const nt::ScoreNote*> notes;
    Ticks start = 0;
    Ticks ticks = 0;
    int staff = 0;
    int voice = 0;
    bool grace = false;
    bool rest = false;
    std::vector<int> slur_starts;
    std::vector<int> slur_stops;
};

enum class DirKind { Dynamic, WedgeStart, WedgeStop, Tempo, Words };

struct Direction {
    Ticks offset = 0;
    int staff = 0;
    DirKind kind = DirKind::Words;
    const nt::DynamicMarking* dynamic = nullptr;
    nt::HairpinKind wedge = nt::HairpinKind::Crescendo;
    int number = 1;
    // Tempo
    double per_minute = 0;
    double sound_tempo = 0;
    nt::NoteValue unit = nt::NoteValue::Quarter;
    int unit_dots = 0;
    std::string text;
    int voice = 0;  ///< xml voice for words, 0 none.
};

struct MeasureOut {
    std::vector<Lane> lanes;
    std::vector<Direction> directions;
};

struct TempoEvent {
    Ticks tick = 0;
    double sound = 0;  ///< Quarter bpm for <sound tempo>.
    double per_minute = 0;
    nt::NoteValue unit = nt::NoteValue::Quarter;
    int unit_dots = 0;
    std::string text;
};

/// Hands out MusicXML numbers (slurs, wedges) so that spans overlapping in
/// time never share one.
class NumberPool {
public:
    int take(Ticks start, Ticks stop)
    {
        for (int n = 1;; ++n) {
            bool busy = false;
            for (const auto& u : used_)
                if (u.first == n && u.second >= start) busy = true;
            if (!busy) {
                used_.emplace_back(n, stop);
                return n;
            }
        }
    }

private:
    std::vector<std::pair<int, Ticks>> used_;
};

// --- The exporter -------------------------------------------------------------

class Exporter {
public:
    Exporter(const project::ProjectData& data, const ExportOptions& opts, std::vector<std::string>* warnings)
        : data_(data), opts_(opts), warnings_(warnings)
    {
    }

    std::string run();

private:
    struct PartCtx {
        const project::Track* track = nullptr;
        const nt::Score* score = nullptr;
        std::string id;
        bool percussion = false;
        std::vector<int> drum_notes;  ///< Distinct sounding notes on percussion staves.
        std::vector<MeasureOut> measures;
        long long divisions = 1;
        long long tick_unit = kTicksPerQuarter;  ///< Ticks per division.
        std::vector<int> written_fifths;          ///< Per staff, the key shown at tick 0.
    };

    void warn(const std::string& w)
    {
        if (warnings_ && std::find(warnings_->begin(), warnings_->end(), w) == warnings_->end())
            warnings_->push_back(w);
    }

    void layout_measures(const std::vector<PartCtx>& parts);
    void build_part(PartCtx& part, bool first_part);
    void write_part_list(xml::Writer& w, const std::vector<PartCtx>& parts);
    void write_part(xml::Writer& w, const PartCtx& part, bool first_part);
    void write_event(xml::Writer& w, const PartCtx& part, const Lane& lane, const Event& e);
    void write_direction(xml::Writer& w, const PartCtx& part, const Direction& d);

    std::size_t measure_index_at(Ticks tick) const
    {
        auto it = std::upper_bound(measures_.begin(), measures_.end(), tick,
                                   [](Ticks t, const MeasureInfo& m) { return t < m.start; });
        if (it == measures_.begin()) return 0;
        return static_cast<std::size_t>(it - measures_.begin() - 1);
    }

    /// The measure a span ending at `tick` ends in: a span ending on a
    /// barline ends in the measure before it.
    std::size_t measure_index_ending_at(Ticks tick) const
    {
        std::size_t i = measure_index_at(tick);
        if (i > 0 && measures_[i].start == tick) --i;
        return i;
    }

    nt::Pitch display_pitch(const nt::Staff& staff, const nt::Pitch& p, const nt::KeySignature& concert_key) const
    {
        if (opts_.concert_pitch) {
            const int t = staff.transposition_semitones;
            nt::Pitch out = t != 0 ? detail::transpose_pitch(p, detail::diatonic_for_semitones(t), t, concert_key) : p;
            out.octave = static_cast<std::int8_t>(out.octave + staff.octave_shift);
            return out;
        }
        nt::Pitch out = p;
        out.octave = static_cast<std::int8_t>(out.octave + clef_octave(staff.clef));
        return out;
    }

    int written_fifths(const nt::Staff& staff, int concert_fifths) const
    {
        if (opts_.concert_pitch || staff.transposition_semitones == 0) return concert_fifths;
        const int t = staff.transposition_semitones;
        return detail::normalise_fifths(concert_fifths +
                                        detail::fifths_delta(-detail::diatonic_for_semitones(t), -t));
    }

    nt::KeySignature written_key(const nt::Staff& staff, const nt::KeySignature& concert) const
    {
        nt::KeySignature k = concert;
        k.fifths = static_cast<std::int8_t>(written_fifths(staff, concert.fifths));
        return k;
    }

    std::string mode_for(const nt::KeySignature& key, Ticks tick) const
    {
        if (tick == 0) {
            // The project key can name a church mode the score cannot hold.
            const auto space = data_.key.find(' ');
            if (space != std::string::npos) {
                std::string m = data_.key.substr(space + 1);
                for (char& c : m) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                static const char* kModes[] = {"major", "minor", "dorian", "phrygian", "lydian",
                                               "mixolydian", "aeolian", "ionian", "locrian"};
                for (const char* k : kModes)
                    if (m == k) {
                        const bool minorish = m == "minor" || m == "aeolian" || m == "dorian" || m == "phrygian" ||
                                              m == "locrian";
                        if (minorish == key.is_minor) return m;
                    }
            }
        }
        return key.is_minor ? "minor" : "major";
    }

    const project::ProjectData& data_;
    const ExportOptions& opts_;
    std::vector<std::string>* warnings_;
    std::vector<MeasureInfo> measures_;
    bool pickup_ = false;
    std::vector<TempoEvent> tempos_;
};

void Exporter::layout_measures(const std::vector<PartCtx>& parts)
{
    Ticks end = 0;
    for (const auto& p : parts) end = std::max(end, p.score->end_tick());
    for (const auto& t : tempos_) end = std::max(end, t.tick + 1);
    int bars = end > 0 ? data_.meter.bar_at(end - 1) : 1;
    const auto& changes = data_.meter.changes();
    if (!changes.empty()) bars = std::max(bars, changes.back().bar);
    bars = std::max(bars, 1);
    if (opts_.detect_pickup && changes.size() >= 2 && changes[1].bar == 2 &&
        changes[0].signature.ticks_per_bar() < changes[1].signature.ticks_per_bar()) {
        pickup_ = true;
        bars = std::max(bars, 2);
    }
    measures_.clear();
    for (int b = 1; b <= bars; ++b) {
        MeasureInfo m;
        m.bar = b;
        m.start = data_.meter.bar_start(b);
        m.sig = data_.meter.at_bar(b);
        m.length = m.sig.ticks_per_bar();
        measures_.push_back(m);
    }
}

void Exporter::build_part(PartCtx& part, bool first_part)
{
    const nt::Score& score = *part.score;
    part.measures.assign(measures_.size(), MeasureOut{});
    const std::size_t staves = score.staff_count();

    for (std::size_t s = 0; s < staves; ++s)
        if (score.staff(s).clef == nt::Clef::Percussion) part.percussion = true;

    // --- Chords per staff and voice, with tie and slur bookkeeping ---------
    std::set<nt::EventId> tie_stopped;
    std::vector<std::vector<Chord>> chords(staves);
    bool cross_staff = false;
    for (std::size_t s = 0; s < staves; ++s) {
        const auto& notes = score.notes(s);
        std::vector<const nt::ScoreNote*> order;
        for (const auto& n : notes) {
            if (!n.is_valid()) continue;
            order.push_back(&n);
            if (n.staff_shift != 0) cross_staff = true;
            if (n.tied_to_next && !n.is_rest) {
                const int m = nt::midi_note_number(n.pitch);
                for (const auto& o : notes)
                    if (&o != &n && !o.grace && !o.is_rest && o.voice == n.voice && o.start_tick == n.end_tick() &&
                        nt::midi_note_number(o.pitch) == m) {
                        tie_stopped.insert(o.id);
                        break;
                    }
            }
            if (part.percussion && !n.is_rest && score.staff(s).clef == nt::Clef::Percussion) {
                const int midi = nt::midi_note_number(n.pitch) + score.staff(s).total_transposition();
                if (std::find(part.drum_notes.begin(), part.drum_notes.end(), midi) == part.drum_notes.end())
                    part.drum_notes.push_back(midi);
            }
        }
        std::stable_sort(order.begin(), order.end(), [](const nt::ScoreNote* a, const nt::ScoreNote* b) {
            if (a->voice != b->voice) return a->voice < b->voice;
            if (a->start_tick != b->start_tick) return a->start_tick < b->start_tick;
            if (a->grace != b->grace) return a->grace;
            if (a->grace_order != b->grace_order) return a->grace_order < b->grace_order;
            return false;
        });
        for (const nt::ScoreNote* n : order) {
            const Ticks len = n->grace ? 0 : n->duration.ticks();
            if (!chords[s].empty()) {
                Chord& c = chords[s].back();
                const nt::ScoreNote* f = c.notes.front();
                if (!n->is_rest && !c.rest && c.voice == n->voice && c.start == n->start_tick && c.grace == n->grace &&
                    (!n->grace || f->grace_order == n->grace_order) && c.ticks == len) {
                    if (f->duration != n->duration)
                        warn("chord notes with different written values were written with the first note's value");
                    c.notes.push_back(n);
                    continue;
                }
            }
            Chord c;
            c.notes.push_back(n);
            c.start = n->start_tick;
            c.ticks = len;
            c.staff = static_cast<int>(s);
            c.voice = n->voice;
            c.grace = n->grace;
            c.rest = n->is_rest;
            chords[s].push_back(std::move(c));
        }
    }
    if (part.percussion) std::sort(part.drum_notes.begin(), part.drum_notes.end());
    if (cross_staff) warn("cross-staff notes were written on their own staff");

    // --- Slurs ------------------------------------------------------------------
    {
        NumberPool pool;
        std::vector<const nt::Slur*> slurs;
        for (const auto& sl : score.slurs()) slurs.push_back(&sl);
        std::stable_sort(slurs.begin(), slurs.end(),
                         [](const nt::Slur* a, const nt::Slur* b) { return a->start_tick < b->start_tick; });
        for (const nt::Slur* sl : slurs) {
            if (sl->staff >= staves || !sl->is_valid()) continue;
            Chord* first = nullptr;
            Chord* last = nullptr;
            for (auto& c : chords[sl->staff]) {
                if (c.voice != sl->voice || c.grace || c.rest) continue;
                if (c.start >= sl->start_tick && c.start < sl->end_tick) {
                    if (!first) first = &c;
                    last = &c;
                }
            }
            if (!first || first == last) {
                warn("slurs that do not join two notes were left out");
                continue;
            }
            const int number = pool.take(first->start, last->start);
            first->slur_starts.push_back(number);
            last->slur_stops.push_back(number);
        }
    }

    // --- Split chords at barlines into events -------------------------------------
    std::map<std::pair<int, int>, std::vector<std::vector<Event>>> per_voice;  // (staff, voice) -> per measure
    auto events_for = [&](int staff, int voice) -> std::vector<std::vector<Event>>& {
        auto& v = per_voice[{staff, voice}];
        if (v.empty()) v.resize(measures_.size());
        return v;
    };
    bool untypeable = false;
    for (std::size_t s = 0; s < staves; ++s) {
        const nt::Staff& staff = score.staff(s);
        const bool perc = staff.clef == nt::Clef::Percussion;
        for (auto& c : chords[s]) {
            const nt::ScoreNote* f = c.notes.front();
            auto& lanes = events_for(c.staff, c.voice);
            auto make_pieces = [&](bool first_seg, bool last_seg, bool first_piece, bool last_piece) {
                std::vector<Piece> out;
                for (const nt::ScoreNote* n : c.notes) {
                    Piece p;
                    p.src = n;
                    if (!n->is_rest) {
                        p.midi = nt::midi_note_number(n->pitch) + staff.total_transposition();
                        p.pitch = perc ? drum_display(p.midi)
                                       : display_pitch(staff, n->pitch, score.key_signature_at(n->start_tick));
                        p.tie_stop = !first_piece || (first_seg && tie_stopped.count(n->id) > 0);
                        p.tie_start = !last_piece || (last_seg && n->tied_to_next);
                    }
                    p.first = first_piece;
                    out.push_back(p);
                }
                return out;
            };
            if (c.grace) {
                const std::size_t mi = measure_index_at(c.start);
                Event e;
                e.offset = c.start - measures_[mi].start;
                e.ticks = 0;
                e.dur = f->duration;
                e.grace = true;
                e.slash = f->grace_slashed;
                e.grace_order = f->grace_order;
                e.notes = make_pieces(true, true, true, true);
                lanes[mi].push_back(std::move(e));
                continue;
            }
            const Ticks end = c.start + c.ticks;
            std::size_t mi = measure_index_at(c.start);
            const bool single = measures_[mi].start + measures_[mi].length >= end;
            Ticks pos = c.start;
            bool first_seg = true;
            while (pos < end && mi < measures_.size()) {
                const MeasureInfo& m = measures_[mi];
                const Ticks seg_end = std::min(end, m.start + m.length);
                const Ticks seg = seg_end - pos;
                const bool last_seg = seg_end >= end;
                std::vector<nt::Duration> durs;
                if (single) {
                    durs.push_back(f->duration);
                } else {
                    // Decompose the segment, keeping the note's tuplet when the
                    // segment is a whole number of its tuplet units.
                    nt::Duration buf[16];
                    const nt::Duration& d = f->duration;
                    const long long num = static_cast<long long>(d.tuplet.actual) * d.outer.actual;
                    const long long den = static_cast<long long>(d.tuplet.normal) * d.outer.normal;
                    std::size_t k = 0;
                    if (d.tuplet.is_tuplet() && (seg * num) % den == 0) {
                        k = nt::decompose_ticks(seg * num / den, buf, 16);
                        for (std::size_t i = 0; i < k; ++i) {
                            buf[i].tuplet = d.tuplet;
                            buf[i].outer = d.outer;
                            if (!buf[i].is_valid()) k = 0;
                        }
                    }
                    if (k == 0) k = nt::decompose_ticks(seg, buf, 16);
                    Ticks sum = 0;
                    for (std::size_t i = 0; i < k; ++i) sum += buf[i].ticks();
                    if (k == 0 || sum != seg) {
                        durs.clear();
                        durs.push_back(f->duration);  // ticks fixed below; type may not match
                    } else {
                        durs.assign(buf, buf + k);
                    }
                }
                Ticks p = pos;
                for (std::size_t i = 0; i < durs.size(); ++i) {
                    Event e;
                    e.offset = p - m.start;
                    e.dur = durs[i];
                    e.ticks = durs.size() == 1 ? seg : durs[i].ticks();
                    e.exact = e.dur.ticks() == e.ticks;
                    if (!e.exact) untypeable = true;
                    e.rest = c.rest;
                    const bool first_piece = first_seg && i == 0;
                    const bool last_piece = last_seg && i + 1 == durs.size();
                    e.notes = make_pieces(first_seg && i == 0, last_seg && i + 1 == durs.size(), first_piece, last_piece);
                    if (c.rest)
                        for (auto& pc : e.notes) pc.tie_start = pc.tie_stop = false;
                    if (first_piece) e.slur_starts = c.slur_starts;
                    if (last_piece) e.slur_stops = c.slur_stops;
                    p += e.ticks;
                    lanes[mi].push_back(std::move(e));
                }
                pos = seg_end;
                first_seg = false;
                ++mi;
            }
        }
    }
    if (untypeable) warn("some durations have no single written value and were written without <type>");

    // --- Lanes per measure ------------------------------------------------------------
    int overflow_voice = static_cast<int>(staves) * 4;
    bool overflow = false;
    for (std::size_t mi = 0; mi < measures_.size(); ++mi) {
        MeasureOut& out = part.measures[mi];
        for (auto& [key, per_measure] : per_voice) {
            auto& evs = per_measure[mi];
            if (evs.empty()) continue;
            std::stable_sort(evs.begin(), evs.end(), [](const Event& a, const Event& b) {
                if (a.offset != b.offset) return a.offset < b.offset;
                if (a.grace != b.grace) return a.grace;
                return a.grace_order < b.grace_order;
            });
            std::vector<Lane> lanes;
            for (auto& e : evs) {
                Lane* target = nullptr;
                for (auto& l : lanes)
                    if (l.end <= e.offset) {
                        target = &l;
                        break;
                    }
                if (!target) {
                    Lane l;
                    l.staff = key.first;
                    l.voice = lanes.empty() ? key.second : -1;
                    if (lanes.empty() && key.second < 4) {
                        l.xml_voice = key.first * 4 + key.second + 1;
                    } else {
                        l.xml_voice = ++overflow_voice;
                        if (!lanes.empty()) overflow = true;
                    }
                    lanes.push_back(std::move(l));
                    target = &lanes.back();
                }
                target->end = e.offset + e.ticks;
                target->events.push_back(std::move(e));
            }
            for (auto& l : lanes) out.lanes.push_back(std::move(l));
        }
        // Empty staves get a whole-measure rest.
        for (std::size_t s = 0; s < staves; ++s) {
            bool has = false;
            for (const auto& l : out.lanes)
                if (l.staff == static_cast<int>(s)) has = true;
            if (has) continue;
            Lane l;
            l.staff = static_cast<int>(s);
            l.xml_voice = static_cast<int>(s) * 4 + 1;
            Event e;
            e.ticks = measures_[mi].length;
            e.rest = e.measure_rest = true;
            e.exact = false;
            Piece p;
            e.notes.push_back(p);
            l.events.push_back(std::move(e));
            out.lanes.push_back(std::move(l));
        }
        std::stable_sort(out.lanes.begin(), out.lanes.end(), [](const Lane& a, const Lane& b) {
            if (a.staff != b.staff) return a.staff < b.staff;
            return a.xml_voice < b.xml_voice;
        });
    }
    if (overflow) warn("overlapping notes within one voice were written as additional voices");

    // --- Tuplet brackets ----------------------------------------------------------------
    for (auto& mo : part.measures) {
        for (auto& lane : mo.lanes) {
            for (int level = 1; level <= 2; ++level) {
                struct Group {
                    int first = -1, last = -1;
                    nt::Tuplet ratio;
                    long long sum = 0, unit = 0;
                    nt::NoteValue value = nt::NoteValue::Eighth;
                } g;
                auto close = [&]() {
                    if (g.first >= 0 && g.last > g.first) {
                        TupletMark start{level, true, g.ratio, g.value};
                        TupletMark stop{level, false, g.ratio, g.value};
                        lane.events[static_cast<std::size_t>(g.first)].tuplets.push_back(start);
                        lane.events[static_cast<std::size_t>(g.last)].tuplets.push_back(stop);
                    }
                    g = Group{};
                };
                for (std::size_t i = 0; i < lane.events.size(); ++i) {
                    const Event& e = lane.events[i];
                    if (e.grace) continue;
                    const nt::Duration& d = e.dur;
                    nt::Tuplet ratio;
                    long long value = 0, unit = 0;
                    bool member = false;
                    if (level == 1 && d.tuplet.is_tuplet() && e.exact) {
                        ratio = d.is_nested() ? d.outer : d.tuplet;
                        value = e.ticks * ratio.actual / ratio.normal;
                        unit = d.is_nested() ? value * d.tuplet.actual : nt::base_ticks(d.value);
                        member = true;
                    } else if (level == 2 && d.is_nested() && e.exact) {
                        ratio = d.tuplet;
                        value = e.ticks * d.outer.actual * d.tuplet.actual / (d.outer.normal * d.tuplet.normal);
                        unit = nt::base_ticks(d.value);
                        member = true;
                    }
                    if (!member || (g.first >= 0 && ratio != g.ratio)) close();
                    if (!member) continue;
                    if (g.first < 0) {
                        g.first = static_cast<int>(i);
                        g.ratio = ratio;
                        g.unit = unit;
                        g.value = d.value;
                    }
                    g.last = static_cast<int>(i);
                    g.sum += value;
                    if (unit < g.unit) {
                        g.unit = unit;
                        g.value = d.value;
                    }
                    if (g.unit > 0 && g.sum % (g.ratio.actual * g.unit) == 0) close();
                }
                close();
            }
        }
    }

    // --- Accidentals ------------------------------------------------------------------
    if (!part.percussion || staves > 1) {
        for (std::size_t mi = 0; mi < measures_.size(); ++mi) {
            for (std::size_t s = 0; s < staves; ++s) {
                const nt::Staff& staff = score.staff(s);
                if (staff.clef == nt::Clef::Percussion) continue;
                struct Ref {
                    Piece* piece;
                    Ticks offset;
                    bool grace;
                    int order;
                };
                std::vector<Ref> refs;
                for (auto& lane : part.measures[mi].lanes) {
                    if (lane.staff != static_cast<int>(s)) continue;
                    for (auto& e : lane.events)
                        if (!e.rest)
                            for (auto& p : e.notes) refs.push_back({&p, e.offset, e.grace, e.grace_order});
                }
                std::stable_sort(refs.begin(), refs.end(), [](const Ref& a, const Ref& b) {
                    if (a.offset != b.offset) return a.offset < b.offset;
                    if (a.grace != b.grace) return a.grace;
                    return a.order < b.order;
                });
                const nt::KeySignature key = written_key(staff, score.key_signature_at(measures_[mi].start));
                std::map<int, int> state;  // diatonic index -> alteration in force
                for (const Ref& r : refs) {
                    const nt::Pitch& p = r.piece->pitch;
                    const int index = nt::diatonic_index(p);
                    auto it = state.find(index);
                    const int current = it != state.end() ? it->second : key.alteration_for(p.step);
                    if (r.piece->tie_stop) continue;
                    if (current != p.alteration) {
                        r.piece->accidental = p.alteration;
                        state[index] = p.alteration;
                    }
                }
            }
        }
    }

    // --- Directions ----------------------------------------------------------------------
    auto place = [&](Ticks tick, bool ending) {
        const std::size_t mi = ending ? measure_index_ending_at(tick) : measure_index_at(tick);
        return std::make_pair(mi, tick - measures_[mi].start);
    };
    for (const auto& d : score.dynamics()) {
        if (!d.is_valid()) continue;
        auto [mi, off] = place(d.tick, false);
        Direction dir;
        dir.kind = DirKind::Dynamic;
        dir.offset = off;
        dir.staff = std::min<int>(d.staff, static_cast<int>(staves) - 1);
        dir.dynamic = &d;
        part.measures[mi].directions.push_back(dir);
    }
    {
        NumberPool pool;
        for (const auto& h : score.hairpins()) {
            if (!h.is_valid()) continue;
            const int number = pool.take(h.start_tick, h.end_tick);
            auto [mi, off] = place(h.start_tick, false);
            Direction dir;
            dir.kind = DirKind::WedgeStart;
            dir.offset = off;
            dir.staff = std::min<int>(h.staff, static_cast<int>(staves) - 1);
            dir.wedge = h.kind;
            dir.number = number;
            part.measures[mi].directions.push_back(dir);
            auto [mi2, off2] = place(h.end_tick, true);
            dir.kind = DirKind::WedgeStop;
            dir.offset = off2;
            part.measures[mi2].directions.push_back(dir);
        }
    }
    for (const auto& t : score.texts()) {
        if (!t.is_valid()) continue;
        auto [mi, off] = place(t.tick, false);
        Direction dir;
        dir.kind = DirKind::Words;
        dir.offset = off;
        dir.text = t.text;
        dir.voice = t.voice > 0 && t.voice < 4 ? t.voice + 1 : 0;
        part.measures[mi].directions.push_back(dir);
    }
    if (first_part) {
        for (const auto& t : tempos_) {
            auto [mi, off] = place(t.tick, false);
            Direction dir;
            dir.kind = DirKind::Tempo;
            dir.offset = off;
            dir.per_minute = t.per_minute;
            dir.sound_tempo = t.sound;
            dir.unit = t.unit;
            dir.unit_dots = t.unit_dots;
            dir.text = t.text;
            part.measures[mi].directions.push_back(dir);
        }
    }
    for (auto& mo : part.measures)
        std::stable_sort(mo.directions.begin(), mo.directions.end(),
                         [](const Direction& a, const Direction& b) { return a.offset < b.offset; });

    // --- Divisions ----------------------------------------------------------------------
    long long g = kTicksPerQuarter;
    for (std::size_t mi = 0; mi < measures_.size(); ++mi) {
        g = detail::gcd_ll(g, measures_[mi].length);
        for (const auto& lane : part.measures[mi].lanes)
            for (const auto& e : lane.events) {
                g = detail::gcd_ll(g, e.offset);
                g = detail::gcd_ll(g, e.ticks);
            }
        for (const auto& d : part.measures[mi].directions) g = detail::gcd_ll(g, d.offset);
    }
    if (g <= 0) g = 1;
    part.tick_unit = g;
    part.divisions = kTicksPerQuarter / g;

    for (std::size_t s = 0; s < staves; ++s)
        part.written_fifths.push_back(written_fifths(score.staff(s), score.key_signature_at(0).fifths));
}

void Exporter::write_part_list(xml::Writer& w, const std::vector<PartCtx>& parts)
{
    w.open("part-list");
    for (const auto& p : parts) {
        const project::Track& t = *p.track;
        w.open("score-part", {{"id", p.id}});
        w.leaf("part-name", t.name);
        std::string abbreviation;
        int program = -1;
        if (t.assignment.status == project::ResolutionStatus::Placeholder) program = t.assignment.placeholder_program;
        auto info = opts_.part_info.find(t.id);
        if (info != opts_.part_info.end()) {
            abbreviation = info->second.abbreviation;
            if (info->second.midi_program >= 0) program = info->second.midi_program;
        }
        if (!abbreviation.empty()) w.leaf("part-abbreviation", abbreviation);
        if (p.percussion && !p.drum_notes.empty()) {
            for (int n : p.drum_notes) {
                w.open("score-instrument", {{"id", p.id + "-I" + std::to_string(n + 1)}});
                w.leaf("instrument-name", drum_name(n));
                w.close();
            }
            for (int n : p.drum_notes) {
                w.open("midi-instrument", {{"id", p.id + "-I" + std::to_string(n + 1)}});
                w.leaf("midi-channel", "10");
                w.leaf("midi-unpitched", std::to_string(std::clamp(n, 0, 127) + 1));
                w.close();
            }
        } else {
            w.open("score-instrument", {{"id", p.id + "-I1"}});
            w.leaf("instrument-name", t.name);
            w.close();
            w.open("midi-instrument", {{"id", p.id + "-I1"}});
            w.leaf("midi-channel", std::to_string(p.percussion ? 10 : std::clamp(t.midi_channel, 0, 15) + 1));
            if (program >= 0 && !p.percussion) w.leaf("midi-program", std::to_string(std::clamp(program, 0, 127) + 1));
            w.close();
        }
        w.close();
    }
    w.close();
}

void Exporter::write_event(xml::Writer& w, const PartCtx& part, const Lane& lane, const Event& e)
{
    const nt::Score& score = *part.score;
    const bool multi = score.staff_count() > 1;
    const bool perc_staff = score.staff(static_cast<std::size_t>(lane.staff)).clef == nt::Clef::Percussion;
    for (std::size_t i = 0; i < e.notes.size(); ++i) {
        const Piece& p = e.notes[i];
        w.open("note");
        if (e.grace) {
            if (e.slash) w.empty("grace", {{"slash", "yes"}});
            else w.empty("grace");
        }
        if (i > 0) w.empty("chord");
        if (e.rest) {
            if (e.measure_rest) w.empty("rest", {{"measure", "yes"}});
            else w.empty("rest");
        } else if (perc_staff) {
            w.open("unpitched");
            w.leaf("display-step", nt::to_string(p.pitch.step));
            w.leaf("display-octave", std::to_string(p.pitch.octave));
            w.close();
        } else {
            w.open("pitch");
            w.leaf("step", nt::to_string(p.pitch.step));
            if (p.pitch.alteration != 0) w.leaf("alter", std::to_string(p.pitch.alteration));
            w.leaf("octave", std::to_string(p.pitch.octave));
            w.close();
        }
        if (!e.grace) w.leaf("duration", std::to_string(e.ticks / part.tick_unit));
        if (p.tie_stop) w.empty("tie", {{"type", "stop"}});
        if (p.tie_start) w.empty("tie", {{"type", "start"}});
        if (perc_staff && !e.rest && part.percussion && !part.drum_notes.empty())
            w.empty("instrument", {{"id", part.id + "-I" + std::to_string(p.midi + 1)}});
        w.leaf("voice", std::to_string(lane.xml_voice));
        if (!e.measure_rest && e.exact) {
            w.leaf("type", detail::type_name(e.dur.value));
            for (int d = 0; d < e.dur.dots; ++d) w.empty("dot");
        }
        if (p.accidental != INT_MIN) w.leaf("accidental", accidental_name(p.accidental));
        if (e.exact && e.dur.tuplet.is_tuplet()) {
            const unsigned actual = static_cast<unsigned>(e.dur.tuplet.actual) * e.dur.outer.actual;
            const unsigned normal = static_cast<unsigned>(e.dur.tuplet.normal) * e.dur.outer.normal;
            w.open("time-modification");
            w.leaf("actual-notes", std::to_string(actual));
            w.leaf("normal-notes", std::to_string(normal));
            w.close();
        }
        if (multi) w.leaf("staff", std::to_string(lane.staff + 1));

        // Notations.
        const nt::ArticulationMask a = p.first && p.src ? p.src->articulations : 0;
        auto has = [&](nt::Articulation x) { return nt::has_articulation(a, x); };
        const bool first_note = i == 0;
        const bool any_notation = p.tie_start || p.tie_stop || a != 0 ||
                                  (first_note && (!e.tuplets.empty() || !e.slur_starts.empty() || !e.slur_stops.empty()));
        if (any_notation) {
            w.open("notations");
            if (p.tie_stop) w.empty("tied", {{"type", "stop"}});
            if (p.tie_start) w.empty("tied", {{"type", "start"}});
            if (first_note) {
                for (int n : e.slur_stops) w.empty("slur", {{"type", "stop"}, {"number", std::to_string(n)}});
                for (int n : e.slur_starts)
                    w.empty("slur", {{"type", "start"}, {"number", std::to_string(n)}});
                for (const auto& t : e.tuplets) {
                    if (!t.start) {
                        w.empty("tuplet", {{"type", "stop"}, {"number", std::to_string(t.number)}});
                        continue;
                    }
                    xml::Attributes attrs{{"type", "start"}, {"number", std::to_string(t.number)}, {"bracket", "yes"}};
                    if (e.dur.is_nested()) {
                        w.open("tuplet", attrs);
                        w.open("tuplet-actual");
                        w.leaf("tuplet-number", std::to_string(t.ratio.actual));
                        w.leaf("tuplet-type", detail::type_name(t.unit));
                        w.close();
                        w.open("tuplet-normal");
                        w.leaf("tuplet-number", std::to_string(t.ratio.normal));
                        w.leaf("tuplet-type", detail::type_name(t.unit));
                        w.close();
                        w.close();
                    } else {
                        w.empty("tuplet", attrs);
                    }
                }
            }
            const bool orn = has(nt::Articulation::Trill) || has(nt::Articulation::Mordent) ||
                             has(nt::Articulation::Turn) || has(nt::Articulation::Tremolo);
            if (orn) {
                w.open("ornaments");
                if (has(nt::Articulation::Trill)) w.empty("trill-mark");
                if (has(nt::Articulation::Mordent)) w.empty("mordent");
                if (has(nt::Articulation::Turn)) w.empty("turn");
                if (has(nt::Articulation::Tremolo)) w.leaf("tremolo", "3", {{"type", "single"}});
                w.close();
            }
            const bool tech = has(nt::Articulation::UpBow) || has(nt::Articulation::DownBow) ||
                              has(nt::Articulation::Harmonic) || has(nt::Articulation::Pizzicato) ||
                              has(nt::Articulation::Mute);
            if (tech) {
                w.open("technical");
                if (has(nt::Articulation::UpBow)) w.empty("up-bow");
                if (has(nt::Articulation::DownBow)) w.empty("down-bow");
                if (has(nt::Articulation::Harmonic)) w.empty("harmonic");
                if (has(nt::Articulation::Mute)) w.empty("stopped");
                if (has(nt::Articulation::Pizzicato)) w.leaf("other-technical", "pizz.");
                w.close();
            }
            const bool art = has(nt::Articulation::Accent) || has(nt::Articulation::Staccato) ||
                             has(nt::Articulation::Staccatissimo) || has(nt::Articulation::Tenuto) ||
                             has(nt::Articulation::Marcato) || has(nt::Articulation::Portato) ||
                             has(nt::Articulation::Spiccato) || has(nt::Articulation::Legato) ||
                             has(nt::Articulation::Falloff) || has(nt::Articulation::Doit) ||
                             has(nt::Articulation::Plop) || has(nt::Articulation::Scoop);
            if (art) {
                w.open("articulations");
                if (has(nt::Articulation::Accent)) w.empty("accent");
                if (has(nt::Articulation::Marcato)) w.empty("strong-accent", {{"type", "up"}});
                if (has(nt::Articulation::Staccato)) w.empty("staccato");
                if (has(nt::Articulation::Tenuto)) w.empty("tenuto");
                if (has(nt::Articulation::Portato)) w.empty("detached-legato");
                if (has(nt::Articulation::Staccatissimo)) w.empty("staccatissimo");
                if (has(nt::Articulation::Spiccato)) w.empty("spiccato");
                if (has(nt::Articulation::Scoop)) w.empty("scoop");
                if (has(nt::Articulation::Plop)) w.empty("plop");
                if (has(nt::Articulation::Doit)) w.empty("doit");
                if (has(nt::Articulation::Falloff)) w.empty("falloff");
                if (has(nt::Articulation::Legato)) w.leaf("other-articulation", "legato");
                w.close();
            }
            if (has(nt::Articulation::Fermata)) w.empty("fermata", {{"type", "upright"}});
            w.close();
        }
        w.close();
    }
}

void Exporter::write_direction(xml::Writer& w, const PartCtx& part, const Direction& d)
{
    const bool multi = part.score->staff_count() > 1;
    const bool above = d.kind == DirKind::Tempo || d.kind == DirKind::Words;
    w.open("direction", {{"placement", above ? "above" : "below"}});
    switch (d.kind) {
        case DirKind::Dynamic: {
            w.open("direction-type");
            w.open("dynamics");
            if (d.dynamic->text.empty()) w.empty(nt::to_string(d.dynamic->level));
            else w.leaf("other-dynamics", d.dynamic->text);
            w.close();
            w.close();
            break;
        }
        case DirKind::WedgeStart:
        case DirKind::WedgeStop: {
            w.open("direction-type");
            const char* type = d.kind == DirKind::WedgeStop          ? "stop"
                               : d.wedge == nt::HairpinKind::Crescendo ? "crescendo"
                                                                       : "diminuendo";
            w.empty("wedge", {{"type", type}, {"number", std::to_string(d.number)}});
            w.close();
            break;
        }
        case DirKind::Tempo: {
            if (!d.text.empty()) {
                w.open("direction-type");
                w.leaf("words", d.text, {{"font-weight", "bold"}});
                w.close();
            }
            if (d.per_minute > 0) {
                w.open("direction-type");
                w.open("metronome", {{"parentheses", "no"}});
                w.leaf("beat-unit", detail::type_name(d.unit));
                for (int i = 0; i < d.unit_dots; ++i) w.empty("beat-unit-dot");
                w.leaf("per-minute", format_number(d.per_minute));
                w.close();
                w.close();
            }
            break;
        }
        case DirKind::Words: {
            w.open("direction-type");
            w.leaf("words", d.text);
            w.close();
            break;
        }
    }
    if (d.voice > 0) w.leaf("voice", std::to_string(d.voice));
    if (multi) w.leaf("staff", std::to_string(d.staff + 1));
    if (d.kind == DirKind::Tempo && d.sound_tempo > 0) w.empty("sound", {{"tempo", format_number(d.sound_tempo)}});
    w.close();
}

void Exporter::write_part(xml::Writer& w, const PartCtx& part, bool first_part)
{
    (void)first_part;
    const nt::Score& score = *part.score;
    const std::size_t staves = score.staff_count();
    w.open("part", {{"id", part.id}});
    TimeSignature shown{0, 0};
    for (std::size_t mi = 0; mi < measures_.size(); ++mi) {
        const MeasureInfo& m = measures_[mi];
        const MeasureOut& mo = part.measures[mi];
        xml::Attributes mattrs;
        if (pickup_) {
            mattrs.emplace_back("number", std::to_string(m.bar - 1));
            if (mi == 0) mattrs.emplace_back("implicit", "yes");
        } else {
            mattrs.emplace_back("number", std::to_string(m.bar));
        }
        w.open("measure", mattrs);

        // Attributes.
        const TimeSignature sig = (pickup_ && mi == 0) ? measures_[1].sig : m.sig;
        const bool time_change = sig != shown;
        std::vector<const nt::KeySignatureChange*> keys;
        for (const auto& k : score.key_signature_changes()) {
            const bool here = mi == 0 ? k.tick < m.start + m.length && k.tick <= m.start
                                      : k.tick >= m.start && k.tick < m.start + m.length;
            if (here && (mi == 0 || k.tick > 0)) keys.push_back(&k);
        }
        if (mi == 0 && keys.size() > 1) keys.erase(keys.begin(), keys.end() - 1);
        if (mi == 0 || time_change || !keys.empty()) {
            w.open("attributes");
            if (mi == 0) w.leaf("divisions", std::to_string(part.divisions));
            if (!keys.empty()) {
                const nt::KeySignatureChange& k = *keys.back();
                if (k.tick != m.start) warn("a key change inside a bar was written at the start of the bar");
                w.open("key");
                w.leaf("fifths", std::to_string(written_fifths(score.staff(0), k.key.fifths)));
                w.leaf("mode", mode_for(k.key, k.tick));
                w.close();
            }
            if (time_change) {
                w.open("time");
                w.leaf("beats", std::to_string(sig.numerator));
                w.leaf("beat-type", std::to_string(sig.denominator));
                w.close();
                shown = sig;
            }
            if (mi == 0) {
                if (staves > 1) w.leaf("staves", std::to_string(staves));
                for (std::size_t s = 0; s < staves; ++s)
                    write_clef(w, score.staff(s).clef, staves > 1 ? static_cast<int>(s) + 1 : 0);
                if (!opts_.concert_pitch) {
                    bool same = true;
                    for (std::size_t s = 1; s < staves; ++s)
                        same = same && score.staff(s).transposition_semitones == score.staff(0).transposition_semitones &&
                               score.staff(s).octave_shift - clef_octave(score.staff(s).clef) ==
                                   score.staff(0).octave_shift - clef_octave(score.staff(0).clef);
                    for (std::size_t s = 0; s < (same ? std::size_t{1} : staves); ++s) {
                        const nt::Staff& st = score.staff(s);
                        const int extra = st.octave_shift - clef_octave(st.clef);
                        const int chromatic = st.transposition_semitones + 12 * extra;
                        if (chromatic == 0) continue;
                        const int diatonic = detail::diatonic_for_semitones(st.transposition_semitones) + 7 * extra;
                        const int oc = chromatic / 12;
                        xml::Attributes ta;
                        if (!same) ta.emplace_back("number", std::to_string(s + 1));
                        w.open("transpose", ta);
                        w.leaf("diatonic", std::to_string(diatonic - 7 * oc));
                        w.leaf("chromatic", std::to_string(chromatic - 12 * oc));
                        if (oc != 0) w.leaf("octave-change", std::to_string(oc));
                        w.close();
                    }
                }
            }
            w.close();
        }

        // Directions at the downbeat come first.
        Ticks cursor = 0;
        std::size_t di = 0;
        while (di < mo.directions.size() && mo.directions[di].offset == 0) write_direction(w, part, mo.directions[di++]);

        for (const Lane& lane : mo.lanes) {
            if (cursor > 0) {
                w.open("backup");
                w.leaf("duration", std::to_string(cursor / part.tick_unit));
                w.close();
                cursor = 0;
            }
            for (const Event& e : lane.events) {
                if (e.offset > cursor) {
                    w.open("forward");
                    w.leaf("duration", std::to_string((e.offset - cursor) / part.tick_unit));
                    w.leaf("voice", std::to_string(lane.xml_voice));
                    if (staves > 1) w.leaf("staff", std::to_string(lane.staff + 1));
                    w.close();
                    cursor = e.offset;
                }
                write_event(w, part, lane, e);
                cursor += e.ticks;
            }
        }
        for (; di < mo.directions.size(); ++di) {
            const Direction& d = mo.directions[di];
            if (d.offset < cursor) {
                w.open("backup");
                w.leaf("duration", std::to_string((cursor - d.offset) / part.tick_unit));
                w.close();
            } else if (d.offset > cursor) {
                w.open("forward");
                w.leaf("duration", std::to_string((d.offset - cursor) / part.tick_unit));
                w.close();
            }
            cursor = d.offset;
            write_direction(w, part, d);
        }
        w.close();
    }
    w.close();
}

std::string Exporter::run()
{
    std::vector<PartCtx> parts;
    for (const project::Track* t : musicxml_export_tracks(data_, opts_)) {
        PartCtx p;
        p.track = t;
        p.score = &t->active_version()->score;
        p.id = "P" + std::to_string(parts.size() + 1);
        parts.push_back(p);
    }
    if (parts.empty()) warn("no tracks to export; the file has no parts");

    // Tempo: the project's tempo map, with the text and beat unit of a
    // score's tempo marking where one is written.
    std::map<Ticks, TempoEvent> tempos;
    for (const auto& c : data_.tempo.changes()) {
        TempoEvent e;
        e.tick = c.tick;
        e.sound = c.bpm;
        e.per_minute = c.bpm;
        tempos[c.tick] = e;
    }
    std::set<Ticks> marked;
    for (const auto& p : parts)
        for (const auto& m : p.score->tempos()) {
            if (!m.is_valid() || !marked.insert(m.tick).second) continue;  // first part's marking wins
            auto it = tempos.find(m.tick);
            TempoEvent e = it != tempos.end() ? it->second : TempoEvent{};
            if (it == tempos.end()) {
                e.tick = m.tick;
                e.sound = m.quarter_notes_per_minute();
            }
            e.per_minute = m.beats_per_minute;
            e.unit = m.beat_unit;
            e.unit_dots = m.beat_unit_dots;
            e.text = m.text;
            tempos[m.tick] = e;
        }
    for (auto& [tick, e] : tempos) tempos_.push_back(e);

    layout_measures(parts);
    for (std::size_t i = 0; i < parts.size(); ++i) build_part(parts[i], i == 0);

    xml::Writer w;
    w.declaration();
    w.raw_line(
        "<!DOCTYPE score-partwise PUBLIC \"-//Recordare//DTD MusicXML 4.0 Partwise//EN\" "
        "\"http://www.musicxml.org/dtds/partwise.dtd\">");
    w.open("score-partwise", {{"version", "4.0"}});
    w.open("work");
    w.leaf("work-title", data_.production.title.empty() ? data_.name : data_.production.title);
    w.close();
    w.leaf("movement-title", data_.name);
    w.open("identification");
    if (!data_.production.composer.empty()) w.leaf("creator", data_.production.composer, {{"type", "composer"}});
    w.open("encoding");
    w.leaf("software", "VC Composer 2");
    if (!opts_.encoding_date.empty()) w.leaf("encoding-date", opts_.encoding_date);
    w.empty("supports", {{"element", "accidental"}, {"type", "yes"}});
    w.empty("supports", {{"element", "transpose"}, {"type", opts_.concert_pitch ? "no" : "yes"}});
    w.close();
    w.close();
    write_part_list(w, parts);
    for (std::size_t i = 0; i < parts.size(); ++i) write_part(w, parts[i], i == 0);
    return w.finish();
}

}  // namespace

std::vector<const project::Track*> musicxml_export_tracks(const project::ProjectData& data, const ExportOptions& options)
{
    std::vector<const project::Track*> out;
    for (const auto& t : data.tracks) {
        if (!options.track_ids.empty()) {
            if (std::find(options.track_ids.begin(), options.track_ids.end(), t.id) == options.track_ids.end())
                continue;
        } else if (!t.include_in_export) {
            continue;
        }
        if (t.kind == project::TrackKind::Folder || t.kind == project::TrackKind::Video ||
            t.kind == project::TrackKind::Audio || t.kind == project::TrackKind::Aux)
            continue;
        if (!t.active_version()) continue;
        out.push_back(&t);
    }
    return out;
}

std::string export_musicxml(const project::ProjectData& data, const ExportOptions& options,
                            std::vector<std::string>* warnings)
{
    try {
        return Exporter(data, options, warnings).run();
    } catch (const std::exception& e) {
        if (warnings) warnings->push_back(std::string("export failed: ") + e.what());
        return {};
    }
}

}  // namespace vcc::interchange
