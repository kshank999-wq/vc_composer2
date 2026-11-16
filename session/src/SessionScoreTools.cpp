// Score tools (Main Spec §7): lyrics, explode and implode.
#include <algorithm>
#include <map>

#include "vcc/session/Session.h"

namespace vcc::session {

namespace nt = notation;

namespace {
template <typename F>
std::function<bool(project::ProjectData&)> on_track(const std::string& id, F f)
{
    return [id, f](project::ProjectData& d) mutable {
        project::Track* t = d.find_track(id);
        return t != nullptr && f(*t);
    };
}
}  // namespace

std::vector<std::string> Session::lyric_syllables(const std::string& text)
{
    std::vector<std::string> out;
    std::string word;
    auto flush_word = [&] {
        if (word.empty()) return;
        // "Hal-le-lu-jah": every syllable but the last keeps its hyphen.
        std::string syllable;
        for (std::size_t i = 0; i < word.size(); ++i) {
            if (word[i] == '-' && !syllable.empty() && i + 1 < word.size()) {
                out.push_back(syllable + "-");
                syllable.clear();
            } else {
                syllable += word[i];
            }
        }
        if (!syllable.empty()) out.push_back(syllable);
        word.clear();
    };
    for (char c : text) {
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') flush_word();
        else word += c;
    }
    flush_word();
    return out;
}

int Session::set_lyrics(const std::string& track_id, const std::vector<nt::EventId>& notes, const std::string& text)
{
    const auto syllables = lyric_syllables(text);
    int placed = 0;
    edit(text.empty() ? "Clear lyrics" : "Lyrics", on_track(track_id, [&](project::Track& t) {
        project::TrackVersion* v = t.active_version();
        if (!v) return false;
        // The chosen notes in time order, one per onset and voice (a chord
        // sings one syllable), never rests or grace notes.
        std::vector<std::pair<nt::Ticks, nt::EventId>> targets;
        std::map<std::pair<nt::Ticks, int>, bool> onsets;
        for (std::size_t s = 0; s < v->score.staff_count(); ++s)
            for (const auto& n : v->score.notes(s)) {
                if (n.is_rest || n.grace || std::find(notes.begin(), notes.end(), n.id) == notes.end()) continue;
                if (onsets[{n.start_tick, static_cast<int>(s) * 16 + n.voice}]) continue;
                onsets[{n.start_tick, static_cast<int>(s) * 16 + n.voice}] = true;
                targets.emplace_back(n.start_tick, n.id);
            }
        std::sort(targets.begin(), targets.end());
        for (std::size_t i = 0; i < targets.size(); ++i) {
            const nt::ScoreNote* n = v->score.find_note(targets[i].second);
            if (!n) continue;
            nt::ScoreNote changed = *n;
            changed.lyric = i < syllables.size() ? syllables[i] : std::string();
            if (v->score.modify_note(n->id, changed) && !changed.lyric.empty()) ++placed;
        }
        return !targets.empty();
    }));
    return placed;
}

std::vector<std::string> Session::explode(const std::string& track_id, Ticks from, Ticks to, int parts)
{
    const project::Track* src = data().find_track(track_id);
    const project::TrackVersion* sv = src ? src->active_version() : nullptr;
    if (!sv || parts < 2 || parts > 16 || to <= from) return {};
    // The chords, top note first.
    std::map<std::pair<Ticks, int>, std::vector<nt::ScoreNote>> chords;
    for (const auto& n : sv->score.notes(0))
        if (!n.is_rest && !n.grace && n.start_tick >= from && n.start_tick < to) chords[{n.start_tick, n.voice}].push_back(n);
    if (chords.empty()) return {};
    for (auto& [at, c] : chords)
        std::sort(c.begin(), c.end(), [](const nt::ScoreNote& a, const nt::ScoreNote& b) {
            return nt::midi_note_number(a.pitch) > nt::midi_note_number(b.pitch);
        });

    const std::string name = src->name;
    const std::string instrument = src->instrument_standard_id;
    const auto before = project_.history_size();
    std::vector<std::string> ids;
    for (int k = 0; k < parts; ++k) ids.push_back(add_track(name + " " + std::to_string(k + 1), instrument));
    edit("Explode", [&](project::ProjectData& d) {
        for (int k = 0; k < parts; ++k) {
            project::Track* t = d.find_track(ids[static_cast<std::size_t>(k)]);
            project::TrackVersion* v = t ? t->active_version() : nullptr;
            if (!v) continue;
            for (const auto& [at, c] : chords) {
                // Part k takes the k-th note from the top; a chord with fewer
                // notes than parts doubles its lowest in the lower parts.
                nt::ScoreNote n = c[std::min<std::size_t>(static_cast<std::size_t>(k), c.size() - 1)];
                n.id = nt::kInvalidEventId;
                n.in_chord = false;
                n.voice = 0;
                n.staff_shift = 0;
                v->score.insert_note(0, n);
            }
        }
        return true;
    });
    project_.squash_undo(project_.history_size() - before, "Explode");
    document_changed();
    return ids;
}

bool Session::implode(const std::vector<std::string>& sources, const std::string& target, Ticks from, Ticks to)
{
    if (sources.empty() || to <= from || !data().find_track(target)) return false;
    // Every source note in range, gathered by onset and value.
    struct Key {
        Ticks start;
        Ticks length;
        bool operator<(const Key& o) const { return start != o.start ? start < o.start : length < o.length; }
    };
    std::map<Key, std::pair<nt::Duration, std::vector<nt::Pitch>>> chords;
    for (const auto& id : sources) {
        const project::Track* t = data().find_track(id);
        const project::TrackVersion* v = t ? t->active_version() : nullptr;
        if (!v) continue;
        for (const auto& n : v->score.notes(0)) {
            if (n.is_rest || n.grace || n.start_tick < from || n.start_tick >= to) continue;
            auto& slot = chords[{n.start_tick, n.duration.ticks()}];
            slot.first = n.duration;
            if (std::none_of(slot.second.begin(), slot.second.end(),
                             [&](const nt::Pitch& p) { return nt::midi_note_number(p) == nt::midi_note_number(n.pitch); }))
                slot.second.push_back(n.pitch);
        }
    }
    if (chords.empty()) return false;
    return edit("Implode", on_track(target, [&](project::Track& t) {
        project::TrackVersion* v = t.active_version();
        if (!v) return false;
        // What was there in the range on the first staff gives way.
        std::vector<nt::EventId> old;
        for (const auto& n : v->score.notes(0))
            if (n.start_tick >= from && n.start_tick < to) old.push_back(n.id);
        for (const auto id : old) v->score.remove_note(id);
        for (const auto& [key, chord] : chords) v->score.insert_chord(0, key.start, chord.first, chord.second, 0);
        return true;
    }));
}

std::vector<std::int32_t> Session::play_order(const std::vector<nt::RepeatSection>& repeats, std::int32_t last_bar)
{
    std::vector<nt::RepeatSection> sections = repeats;
    std::sort(sections.begin(), sections.end(), [](const auto& a, const auto& b) { return a.start_bar < b.start_bar; });
    std::vector<std::int32_t> order;
    std::int32_t bar = 1;
    for (const auto& r : sections) {
        if (r.start_bar < bar || r.end_bar < r.start_bar) continue;  // overlapping: ignored
        for (; bar < r.start_bar && bar <= last_bar; ++bar) order.push_back(bar);
        const std::int32_t first_ending = r.end_bar - r.ending_bars + 1;
        for (std::int32_t pass = 1; pass <= r.times; ++pass) {
            const bool last = pass == r.times;
            for (std::int32_t b = r.start_bar; b <= r.end_bar; ++b) {
                if (last && r.ending_bars > 0 && b >= first_ending) break;
                order.push_back(b);
            }
        }
        bar = r.end_bar + 1;
        if (r.ending_bars > 0)
            for (std::int32_t k = 0; k < r.ending_bars; ++k) order.push_back(bar++);  // the second ending
    }
    for (; bar <= last_bar; ++bar) order.push_back(bar);
    return order;
}

bool Session::set_repeats(std::vector<nt::RepeatSection> repeats)
{
    for (auto& r : repeats) {
        r.start_bar = std::max(1, r.start_bar);
        r.end_bar = std::max(r.start_bar, r.end_bar);
        r.times = std::clamp(r.times, 2, 16);
        r.ending_bars = std::clamp(r.ending_bars, 0, r.end_bar - r.start_bar);
    }
    return edit("Repeats", [&](project::ProjectData& d) {
        if (d.repeats == repeats) return false;
        d.repeats = repeats;
        return true;
    });
}

int Session::unroll_repeats()
{
    const auto& d0 = data();
    if (d0.repeats.empty()) return 0;
    // The last bar anything is written in.
    std::int32_t last_bar = 1;
    for (const auto& r : d0.repeats) last_bar = std::max(last_bar, r.end_bar + r.ending_bars);
    for (const auto& t : d0.tracks)
        if (const auto* v = t.active_version())
            for (std::size_t s = 0; s < v->score.staff_count(); ++s)
                for (const auto& n : v->score.notes(s)) last_bar = std::max(last_bar, d0.meter.bar_at(n.start_tick));
    const auto order = play_order(d0.repeats, last_bar);
    const int added = static_cast<int>(order.size()) - last_bar;

    const bool ok = edit("Unroll repeats", [&](project::ProjectData& d) {
        const MeterMap old_meter = d.meter;
        // The new meter: each written-out bar keeps its source bar's signature.
        MeterMap meter(old_meter.at_bar(order.empty() ? 1 : order.front()));
        for (std::size_t j = 1; j < order.size(); ++j)
            if (!(old_meter.at_bar(order[j]) == old_meter.at_bar(order[j - 1]))) meter.set(static_cast<int>(j) + 1, old_meter.at_bar(order[j]));
        auto shift_for = [&](std::size_t j) { return meter.bar_start(static_cast<int>(j) + 1) - old_meter.bar_start(order[j]); };
        auto copy_range = [&](Ticks tick, auto&& emit) {
            // Every written-out bar whose source holds `tick`.
            for (std::size_t j = 0; j < order.size(); ++j) {
                const Ticks a = old_meter.bar_start(order[j]);
                const Ticks b = a + old_meter.at_bar(order[j]).ticks_per_bar();
                if (tick >= a && tick < b) emit(shift_for(j));
            }
        };
        for (auto& t : d.tracks)
            for (auto& v : t.versions) {
                const nt::Score old = v.score;
                nt::Score fresh;
                fresh.reset_staves(old.staves());
                for (std::size_t s = 0; s < old.staff_count(); ++s)
                    for (nt::ScoreNote n : old.notes(s))
                        copy_range(n.start_tick, [&](Ticks by) {
                            nt::ScoreNote c = n;
                            c.id = nt::kInvalidEventId;
                            c.start_tick += by;
                            fresh.insert_note(s, c);
                        });
                for (auto m : old.dynamics()) copy_range(m.tick, [&](Ticks by) { auto c = m; c.tick += by; fresh.add_dynamic(c); });
                for (auto m : old.hairpins())
                    copy_range(m.start_tick, [&](Ticks by) { auto c = m; c.start_tick += by; c.end_tick += by; fresh.add_hairpin(c); });
                for (auto m : old.slurs())
                    copy_range(m.start_tick, [&](Ticks by) { auto c = m; c.start_tick += by; c.end_tick += by; fresh.add_slur(c); });
                for (auto m : old.texts()) copy_range(m.tick, [&](Ticks by) { auto c = m; c.tick += by; fresh.add_text(c); });
                for (auto m : old.tempos()) copy_range(m.tick, [&](Ticks by) { auto c = m; c.tick += by; fresh.add_tempo(c); });
                v.score = std::move(fresh);
            }
        std::vector<project::ChordMark> chords;
        for (const auto& c : d.chords) copy_range(c.tick, [&](Ticks by) { auto x = c; x.tick += by; chords.push_back(x); });
        std::sort(chords.begin(), chords.end(), [](const auto& a, const auto& b) { return a.tick < b.tick; });
        chords.erase(std::unique(chords.begin(), chords.end(), [](const auto& a, const auto& b) { return a.tick == b.tick; }), chords.end());
        d.chords = std::move(chords);
        d.meter = meter;
        d.repeats.clear();
        return true;
    });
    return ok ? added : 0;
}

}  // namespace vcc::session
