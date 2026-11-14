// Phrase blocks (docs/spec/ui/SPEC.md, "Phrase blocks"): named spans of a
// staff that move with the music written in them.
#include <algorithm>

#include "vcc/interchange/Transposition.h"
#include "vcc/session/Session.h"

namespace vcc::session {

namespace {
project::Phrase* find_phrase(project::TrackVersion& v, const std::string& id)
{
    for (auto& p : v.phrases)
        if (p.id == id) return &p;
    return nullptr;
}

void sort_phrases(project::TrackVersion& v)
{
    std::stable_sort(v.phrases.begin(), v.phrases.end(),
                     [](const project::Phrase& a, const project::Phrase& b) { return a.start < b.start; });
}

bool inside(Ticks t, Ticks from, Ticks to) { return t >= from && t < to; }

/// Notes written in [from, to) on any staff (by start), excluding `except`.
bool has_notes(const notation::Score& score, Ticks from, Ticks to, const std::vector<notation::EventId>& except = {})
{
    for (std::size_t s = 0; s < score.staff_count(); ++s)
        for (const auto& n : score.notes(s))
            if (!n.is_rest && n.start_tick < to && n.end_tick() > from &&
                std::find(except.begin(), except.end(), n.id) == except.end())
                return true;
    return false;
}

std::vector<notation::EventId> notes_in(const notation::Score& score, Ticks from, Ticks to)
{
    std::vector<notation::EventId> ids;
    for (std::size_t s = 0; s < score.staff_count(); ++s)
        for (const auto& n : score.notes(s))
            if (inside(n.start_tick, from, to)) ids.push_back(n.id);
    return ids;
}
}  // namespace

std::string Session::add_phrase(const std::string& track_id, const std::string& name, Ticks from, Ticks to)
{
    if (to <= from || from < 0) return {};
    const std::string id = project_.ids().next("phr");
    const bool ok = edit("New phrase", [&](project::ProjectData& d) {
        auto* t = d.find_track(track_id);
        auto* v = t ? t->active_version() : nullptr;
        if (!v) return false;
        v->phrases.push_back({id, name, from, to});
        sort_phrases(*v);
        return true;
    });
    return ok ? id : std::string{};
}

bool Session::rename_phrase(const std::string& track_id, const std::string& phrase_id, const std::string& name)
{
    return edit("Rename phrase", [&](project::ProjectData& d) {
        auto* t = d.find_track(track_id);
        auto* p = t && t->active_version() ? find_phrase(*t->active_version(), phrase_id) : nullptr;
        if (!p || p->name == name) return false;
        p->name = name;
        return true;
    });
}

bool Session::set_phrase_span(const std::string& track_id, const std::string& phrase_id, Ticks from, Ticks to,
                              const std::string& label)
{
    if (to <= from || from < 0) return false;
    const bool ok = project_.edit_coalescing(label, [&](project::ProjectData& d) {
        auto* t = d.find_track(track_id);
        auto* v = t ? t->active_version() : nullptr;
        auto* p = v ? find_phrase(*v, phrase_id) : nullptr;
        if (!p || (p->start == from && p->end == to)) return false;
        p->start = from;
        p->end = to;
        sort_phrases(*v);
        return true;
    });
    if (ok) document_changed();
    return ok;
}

bool Session::move_phrase(const std::string& track_id, const std::string& phrase_id, Ticks delta, std::string* why)
{
    const auto* t = data().find_track(track_id);
    const auto* v = t ? t->active_version() : nullptr;
    const project::Phrase* p = nullptr;
    if (v)
        for (const auto& ph : v->phrases)
            if (ph.id == phrase_id) p = &ph;
    if (!p || delta == 0) return false;
    if (p->start + delta < 0) {
        if (why) *why = "A phrase cannot start before the beginning";
        return false;
    }
    const auto moving = notes_in(v->score, p->start, p->end);
    if (has_notes(v->score, p->start + delta, p->end + delta, moving)) {
        if (why) *why = "There are notes written where the phrase would land";
        return false;
    }
    const Ticks from = p->start, to = p->end;
    return edit("Move phrase", [&](project::ProjectData& d) {
        auto* tr = d.find_track(track_id);
        auto* ver = tr->active_version();
        auto& score = ver->score;
        for (auto id : moving)
            if (const auto* n = score.find_note(id)) {
                auto moved = *n;
                moved.start_tick += delta;
                score.modify_note(id, moved);
            }
        // The markings written in the phrase travel with it.
        auto dynamics = score.dynamics();
        for (const auto& m : dynamics)
            if (inside(m.tick, from, to)) {
                score.remove_dynamic(m.tick, m.voice, m.staff);
                auto moved = m;
                moved.tick += delta;
                score.add_dynamic(moved);
            }
        auto hairpins = score.hairpins();
        for (const auto& h : hairpins)
            if (inside(h.start_tick, from, to)) {
                score.remove_hairpin(h.start_tick, h.voice, h.staff);
                auto moved = h;
                moved.start_tick += delta;
                moved.end_tick += delta;
                score.add_hairpin(moved);
            }
        auto slurs = score.slurs();
        for (const auto& sl : slurs)
            if (inside(sl.start_tick, from, to)) {
                score.remove_slur(sl);
                auto moved = sl;
                moved.start_tick += delta;
                moved.end_tick += delta;
                score.add_slur(moved);
            }
        auto* ph = find_phrase(*ver, phrase_id);
        ph->start += delta;
        ph->end += delta;
        sort_phrases(*ver);
        return true;
    });
}

bool Session::split_phrase(const std::string& track_id, const std::string& phrase_id, Ticks at)
{
    const std::string id = project_.ids().next("phr");
    return edit("Split phrase", [&](project::ProjectData& d) {
        auto* t = d.find_track(track_id);
        auto* v = t ? t->active_version() : nullptr;
        auto* p = v ? find_phrase(*v, phrase_id) : nullptr;
        if (!p || at <= p->start || at >= p->end) return false;
        project::Phrase second{id, p->name + " (2)", at, p->end};
        p->end = at;
        v->phrases.push_back(second);
        sort_phrases(*v);
        return true;
    });
}

std::string Session::duplicate_phrase(const std::string& track_id, const std::string& phrase_id, std::string* why)
{
    const auto* t = data().find_track(track_id);
    const auto* v = t ? t->active_version() : nullptr;
    const project::Phrase* p = nullptr;
    if (v)
        for (const auto& ph : v->phrases)
            if (ph.id == phrase_id) p = &ph;
    if (!p) return {};
    const Ticks len = p->end - p->start;
    if (has_notes(v->score, p->end, p->end + len)) {
        if (why) *why = "There are notes written right after the phrase";
        return {};
    }
    const std::string id = project_.ids().next("phr");
    const project::Phrase source = *p;
    const bool ok = edit("Duplicate phrase", [&](project::ProjectData& d) {
        auto* ver = d.find_track(track_id)->active_version();
        auto& score = ver->score;
        for (std::size_t s = 0; s < score.staff_count(); ++s) {
            const auto notes = score.notes(s);
            for (const auto& n : notes) {
                if (!inside(n.start_tick, source.start, source.end)) continue;
                auto copy = n;
                copy.start_tick += len;
                const auto new_id = score.insert_note(s, copy);
                // The copy plays as the original does.
                if (auto it = ver->performance.find(n.id); it != ver->performance.end() && new_id)
                    ver->performance[new_id] = it->second;
            }
        }
        for (const auto& m : std::vector<notation::DynamicMarking>(score.dynamics()))
            if (inside(m.tick, source.start, source.end)) {
                auto copy = m;
                copy.tick += len;
                score.add_dynamic(copy);
            }
        for (const auto& h : std::vector<notation::Hairpin>(score.hairpins()))
            if (inside(h.start_tick, source.start, source.end)) {
                auto copy = h;
                copy.start_tick += len;
                copy.end_tick += len;
                score.add_hairpin(copy);
            }
        ver->phrases.push_back({id, source.name, source.end, source.end + len});
        sort_phrases(*ver);
        return true;
    });
    return ok ? id : std::string{};
}

bool Session::remove_phrase(const std::string& track_id, const std::string& phrase_id)
{
    return edit("Remove phrase", [&](project::ProjectData& d) {
        auto* t = d.find_track(track_id);
        auto* v = t ? t->active_version() : nullptr;
        if (!v) return false;
        const auto before = v->phrases.size();
        v->phrases.erase(std::remove_if(v->phrases.begin(), v->phrases.end(),
                                        [&](const project::Phrase& p) { return p.id == phrase_id; }),
                         v->phrases.end());
        return v->phrases.size() != before;
    });
}

bool Session::transpose_notes(const std::string& track_id, const std::vector<notation::EventId>& notes, int semitones)
{
    if (semitones == 0 || notes.empty()) return false;
    return edit(semitones > 0 ? "Transpose up" : "Transpose down", [&](project::ProjectData& d) {
        auto* t = d.find_track(track_id);
        auto* v = t ? t->active_version() : nullptr;
        if (!v) return false;
        auto& score = v->score;
        // The selected notes and the notes tied on from them.
        std::vector<notation::EventId> all;
        for (auto id : notes) {
            const auto* n = score.find_note(id);
            while (n && !n->is_rest) {
                if (std::find(all.begin(), all.end(), n->id) != all.end()) break;
                all.push_back(n->id);
                if (!n->tied_to_next) break;
                const auto staff = score.staff_index_of(n->id);
                const notation::ScoreNote* next = nullptr;
                for (const auto& c : score.notes(staff))
                    if (!c.is_rest && c.voice == n->voice && c.start_tick == n->end_tick() && c.pitch == n->pitch)
                        next = &c;
                n = next;
            }
        }
        bool any = false;
        for (auto id : all) {
            const auto* n = score.find_note(id);
            if (!n) continue;
            const int midi = notation::midi_note_number(n->pitch) + semitones;
            if (midi < 0 || midi > 127) continue;
            const auto staff = score.staff_index_of(id);
            const auto key = interchange::written_key(score.key_signature_at(n->start_tick),
                                                      score.staff(staff).transposition_semitones);
            auto moved = *n;
            moved.pitch = notation::spell_midi_note(midi, key);
            any = score.modify_note(id, moved) || any;
        }
        return any;
    });
}

}  // namespace vcc::session
