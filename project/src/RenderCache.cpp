#include "vcc/project/RenderCache.h"

#include <algorithm>
#include <limits>
#include <map>
#include <set>
#include <tuple>

namespace vcc::project {

namespace nt = vcc::notation;

namespace {

constexpr Ticks kEnd = std::numeric_limits<Ticks>::max() / 4;

bool same(const nt::ScoreNote& a, const nt::ScoreNote& b)
{
    return a.id == b.id && a.start_tick == b.start_tick && a.duration == b.duration && a.pitch == b.pitch &&
           a.is_rest == b.is_rest && a.voice == b.voice && a.tied_to_next == b.tied_to_next &&
           a.articulations == b.articulations && a.in_chord == b.in_chord && a.grace == b.grace &&
           a.grace_order == b.grace_order && a.grace_slashed == b.grace_slashed && a.staff_shift == b.staff_shift;
}

bool same(const nt::DynamicMarking& a, const nt::DynamicMarking& b)
{
    return a.tick == b.tick && a.level == b.level && a.voice == b.voice && a.text == b.text && a.staff == b.staff;
}

bool same(const nt::Hairpin& a, const nt::Hairpin& b)
{
    return a.start_tick == b.start_tick && a.end_tick == b.end_tick && a.kind == b.kind && a.shape == b.shape &&
           a.voice == b.voice && a.staff == b.staff && a.has_start_level == b.has_start_level &&
           a.start_level == b.start_level && a.has_end_level == b.has_end_level && a.end_level == b.end_level;
}

bool same(const NoteEvent& a, const NoteEvent& b) { return a == b && a.articulation == b.articulation; }

bool same_regions(const std::vector<MidiRegion>& a, const std::vector<MidiRegion>& b)
{
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i].id != b[i].id || a[i].notes.size() != b[i].notes.size()) return false;
        for (std::size_t k = 0; k < a[i].notes.size(); ++k)
            if (!same(a[i].notes[k], b[i].notes[k])) return false;
    }
    return true;
}

/// Whether the staves render differently as a whole: count, transposition.
bool staves_differ(const nt::Score& a, const nt::Score& b)
{
    if (a.staff_count() != b.staff_count()) return true;
    for (std::size_t s = 0; s < a.staff_count(); ++s)
        if (a.staff(s).total_transposition() != b.staff(s).total_transposition()) return true;
    return false;
}

/// Each note's tie-chain span (a chain renders as one note from its first).
std::vector<std::pair<Ticks, Ticks>> chain_spans(const std::vector<nt::ScoreNote>& notes)
{
    std::vector<std::size_t> head(notes.size());
    std::vector<Ticks> end(notes.size());
    std::map<std::tuple<int, int, int, int, Ticks>, std::size_t> open;
    for (std::size_t i = 0; i < notes.size(); ++i) {
        const auto& n = notes[i];
        head[i] = i;
        end[i] = n.end_tick();
        if (!n.is_rest && (n.tied_to_next || !open.empty())) {
            const auto key = std::make_tuple(static_cast<int>(n.voice), static_cast<int>(n.pitch.step),
                                             static_cast<int>(n.pitch.octave), static_cast<int>(n.pitch.alteration),
                                             n.start_tick);
            if (auto it = open.find(key); it != open.end()) {
                head[i] = it->second;
                open.erase(it);
            }
            if (n.tied_to_next)
                open[std::make_tuple(static_cast<int>(n.voice), static_cast<int>(n.pitch.step),
                                     static_cast<int>(n.pitch.octave), static_cast<int>(n.pitch.alteration),
                                     n.end_tick())] = head[i];
        }
        end[head[i]] = std::max(end[head[i]], n.end_tick());
    }
    std::vector<std::pair<Ticks, Ticks>> spans(notes.size());
    for (std::size_t i = 0; i < notes.size(); ++i) spans[i] = {notes[head[i]].start_tick, end[head[i]]};
    return spans;
}

/// The renderer's own test for a note it renders in a window.
bool in_window(const nt::ScoreNote& n, Ticks from, Ticks to) { return n.end_tick() > from && n.start_tick < to; }

Ticks bar_before(const nt::Score& s, Ticks t)
{
    const auto m = s.measure_at(std::max<Ticks>(0, t));
    if (m.start_tick <= 0) return -1;  // before the first note: everything from the start
    return s.measure_at(m.start_tick - 1).start_tick;
}

Ticks bar_after(const nt::Score& s, Ticks t)
{
    const auto m = s.measure_at(std::max<Ticks>(0, t));
    const Ticks next = m.start_tick + std::max<Ticks>(1, m.length_ticks);
    const auto n = s.measure_at(next);
    return n.start_tick + std::max<Ticks>(1, n.length_ticks);
}

/// Articulations no rule of the map covers, in notes that sound: what a
/// whole render reports, found without rendering.
std::vector<nt::Articulation> unmapped_in(const nt::Score& score, const nt::ExpressionMap& map)
{
    static const nt::Articulation kChecked[] = {
        nt::Articulation::Accent,    nt::Articulation::Staccato,  nt::Articulation::Staccatissimo,
        nt::Articulation::Tenuto,    nt::Articulation::Marcato,   nt::Articulation::Legato,
        nt::Articulation::Portato,   nt::Articulation::Spiccato,  nt::Articulation::Pizzicato,
        nt::Articulation::Tremolo,   nt::Articulation::Trill,     nt::Articulation::Harmonic,
    };
    // Every articulation on a note that sounds (not a rest, not a tie's
    // continuation, inside MIDI's range), in one pass.
    nt::ArticulationMask used = 0;
    for (std::size_t s = 0; s < score.staff_count(); ++s) {
        const auto& notes = score.notes(s);
        const auto spans = chain_spans(notes);
        const int shift = score.staff(s).total_transposition();
        for (std::size_t i = 0; i < notes.size(); ++i) {
            const auto& n = notes[i];
            if (n.is_rest || n.articulations == 0 || spans[i].first != n.start_tick) continue;
            const int midi = nt::midi_note_number(n.pitch);
            if (midi < 0 || midi + shift < 0 || midi + shift > 127) continue;
            used |= n.articulations;
        }
    }
    std::vector<nt::Articulation> out;
    for (const auto a : kChecked)
        if (map.rule_for(a) == nullptr && nt::has_articulation(used, a)) out.push_back(a);
    std::sort(out.begin(), out.end());
    return out;
}

/// The notes that differ between two versions of a staff. Unchanged
/// leading and trailing runs are skipped without allocating; only the
/// middle is matched by id.
template <typename F>
void staff_changes(const std::vector<nt::ScoreNote>& a, const std::vector<nt::ScoreNote>& b, F&& touch)
{
    std::size_t head = 0;
    while (head < a.size() && head < b.size() && same(a[head], b[head])) ++head;
    if (head == a.size() && head == b.size()) return;
    std::size_t ta = a.size(), tb = b.size();
    while (ta > head && tb > head && same(a[ta - 1], b[tb - 1])) --ta, --tb;
    std::map<nt::EventId, const nt::ScoreNote*> before;
    for (std::size_t i = head; i < ta; ++i) before[a[i].id] = &a[i];
    for (std::size_t i = head; i < tb; ++i) {
        const auto it = before.find(b[i].id);
        if (it == before.end()) {
            touch(b[i]);
        } else {
            if (!same(*it->second, b[i])) touch(*it->second), touch(b[i]);
            before.erase(it);
        }
    }
    for (const auto& [id, n] : before) touch(*n);
}

}  // namespace

const char* to_string(RenderStats::Kind kind) noexcept
{
    switch (kind) {
        case RenderStats::Kind::Unchanged: return "unchanged";
        case RenderStats::Kind::Refinish: return "refinish";
        case RenderStats::Kind::Partial: return "partial";
        case RenderStats::Kind::Full: return "full";
    }
    return "?";
}

void RenderCache::forget(const std::string& track_id) { entries_.erase(track_id); }

void RenderCache::retain(const std::vector<std::string>& keep)
{
    for (auto it = entries_.begin(); it != entries_.end();)
        it = std::find(keep.begin(), keep.end(), it->first) == keep.end() ? entries_.erase(it) : std::next(it);
}

const RenderedTrack& RenderCache::render(const Track& track, bool performance)
{
    static const RenderedTrack kNothing;
    last_ = {};
    const TrackVersion* v = track.active_version();
    if (!v) {
        entries_.erase(track.id);
        return kNothing;
    }
    const auto map = expression_map_named(track.expression_map);
    const int channel = std::clamp(track.midi_channel, 0, 15);
    Entry& e = entries_[track.id];
    const nt::Score& now = v->score;

    bool raw_changed = true;
    const bool fresh = e.channel < 0 || e.version_id != v->id || e.map_name != track.expression_map ||
                       e.channel != channel || staves_differ(e.score, now);
    if (fresh) {
        e.raw = render_score_events(track, map);
        last_.kind = RenderStats::Kind::Full;
        last_.events_rendered = e.raw.events.size();
    } else {
        // What changed, note by note and marking by marking.
        Ticks lo = kEnd, hi = -1;
        std::set<nt::EventId> changed;
        auto touch = [&](const nt::ScoreNote& n) {
            changed.insert(n.id);
            lo = std::min(lo, n.start_tick);
            hi = std::max(hi, n.end_tick());
        };
        for (std::size_t s = 0; s < now.staff_count(); ++s) staff_changes(e.score.notes(s), now.notes(s), touch);
        Ticks marking_from = kEnd;
        auto marks = [&](const auto& xs, const auto& ys, auto tick_of) {
            if (xs.size() == ys.size() &&
                std::equal(xs.begin(), xs.end(), ys.begin(), [](const auto& x, const auto& y) { return same(x, y); }))
                return;
            for (const auto& x : xs)
                if (std::none_of(ys.begin(), ys.end(), [&](const auto& y) { return same(x, y); }))
                    marking_from = std::min(marking_from, tick_of(x));
        };
        auto dyn_tick = [](const nt::DynamicMarking& d) { return d.tick; };
        auto pin_tick = [](const nt::Hairpin& h) { return h.start_tick; };
        marks(e.score.dynamics(), now.dynamics(), dyn_tick);
        marks(now.dynamics(), e.score.dynamics(), dyn_tick);
        marks(e.score.hairpins(), now.hairpins(), pin_tick);
        marks(now.hairpins(), e.score.hairpins(), pin_tick);

        if (changed.empty() && marking_from == kEnd) {
            raw_changed = false;
        } else {
            Ticks from = kEnd, to = -1;
            if (!changed.empty()) {
                from = bar_before(now, lo);
                to = bar_after(now, std::max<Ticks>(lo, hi - 1));
            }
            if (marking_from != kEnd) {
                // A level holds until the next, and a hairpin without an end
                // level reads the next marking: from the change (or the
                // hairpins before it) to the end.
                Ticks m = marking_from;
                for (const nt::Score* sc : {static_cast<const nt::Score*>(&e.score), &now})
                    for (const auto& h : sc->hairpins())
                        if (h.start_tick <= marking_from) m = std::min(m, h.start_tick);
                from = std::min(from, bar_before(now, m));
                to = kEnd;
            }
            // Widen until no tie chain crosses an edge, in either score.
            std::vector<std::pair<Ticks, Ticks>> chains;  // tie chains only
            for (const nt::Score* sc : {static_cast<const nt::Score*>(&e.score), &now})
                for (std::size_t s = 0; s < sc->staff_count(); ++s) {
                    const auto& notes = sc->notes(s);
                    if (std::none_of(notes.begin(), notes.end(), [](const nt::ScoreNote& n) { return n.tied_to_next; }))
                        continue;
                    const auto spans = chain_spans(notes);
                    for (std::size_t i = 0; i < notes.size(); ++i)
                        if (spans[i].first != notes[i].start_tick || spans[i].second != notes[i].end_tick())
                            chains.push_back(spans[i]);
                }
            for (bool grew = true; grew;) {
                grew = false;
                for (const auto& [a, b] : chains)
                            if (b > from && a < to && (a < from || b > to)) {
                                from = std::min(from, a == 0 ? Ticks{-1} : a);
                                to = std::max(to, b);
                                grew = true;
                            }
            }

            // Out: events of notes the window holds (before or after), and
            // marking events at ticks in it.
            std::set<nt::EventId> out_ids = changed;
            for (std::size_t s = 0; s < e.score.staff_count(); ++s)
                for (const auto& n : e.score.notes(s))
                    if (in_window(n, from, to)) out_ids.insert(n.id);
            nt::RenderResult window = render_score_events(track, map, from, to);
            last_.kind = RenderStats::Kind::Partial;
            last_.from = from;
            last_.to = to;
            last_.events_rendered = window.events.size();

            nt::RenderResult merged;
            merged.events.reserve(e.raw.events.size() + window.events.size());
            // Marking events strictly after the window's start: a hairpin
            // ending exactly there sends its last value at that tick but is
            // not one the window renders; markings that changed all lie
            // after `from`.
            auto inside = [&](Ticks t) { return t > from && t < to; };
            for (const auto& ev : e.raw.events) {
                if (ev.source_id == nt::kInvalidEventId ? inside(ev.tick) : out_ids.count(ev.source_id) > 0) continue;
                merged.events.push_back(ev);
            }
            for (const auto& ev : window.events)
                if (ev.source_id != nt::kInvalidEventId || inside(ev.tick)) merged.events.push_back(ev);
            canonical_order(merged.events);
            for (auto id : e.raw.out_of_range_notes)
                if (!out_ids.count(id)) merged.out_of_range_notes.push_back(id);
            merged.out_of_range_notes.insert(merged.out_of_range_notes.end(), window.out_of_range_notes.begin(),
                                             window.out_of_range_notes.end());
            std::sort(merged.out_of_range_notes.begin(), merged.out_of_range_notes.end());
            merged.out_of_range_notes.erase(
                std::unique(merged.out_of_range_notes.begin(), merged.out_of_range_notes.end()),
                merged.out_of_range_notes.end());
            merged.unmapped_articulations = unmapped_in(now, map);
            e.raw = std::move(merged);
        }
    }

    const bool around_changed = !same_regions(e.regions, v->regions) || e.performance != v->performance;
    if (raw_changed || around_changed || fresh) {
        e.finished[0].reset();
        e.finished[1].reset();
    }
    if (!raw_changed && !fresh) last_.kind = around_changed ? RenderStats::Kind::Refinish : RenderStats::Kind::Unchanged;
    auto& slot = e.finished[performance ? 1 : 0];
    if (!slot) {
        slot = finish_render(track, map, e.raw, performance);
        if (last_.kind == RenderStats::Kind::Unchanged) last_.kind = RenderStats::Kind::Refinish;
    }
    if (raw_changed || fresh) e.score = now;
    e.version_id = v->id;
    e.map_name = track.expression_map;
    e.channel = channel;
    if (around_changed || fresh) {
        e.regions = v->regions;
        e.performance = v->performance;
    }
    last_.events_total = e.raw.events.size();
    return *slot;
}

}  // namespace vcc::project
