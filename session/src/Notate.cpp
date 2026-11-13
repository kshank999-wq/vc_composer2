#include "vcc/session/Notate.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <set>

#include "vcc/interchange/Transposition.h"
#include "vcc/music/MidiFile.h"
#include "vcc/notation/MidiImport.h"
#include "vcc/project/Performance.h"
#include "vcc/project/Render.h"

namespace vcc::session {

namespace nt = vcc::notation;

namespace {

/// Straight positions: the 32nd-note grid. Triplet positions: the grid of
/// sixteenth-note triplets (and their halves), which a straight grid misses.
constexpr Ticks kStraight = kTicksPerQuarter / 8;  // 360
constexpr Ticks kTriplet = kTicksPerQuarter / 12;  // 240

bool on_straight(Ticks t) { return t % kStraight == 0; }
bool on_triplet(Ticks t) { return t % kTriplet == 0; }

Ticks nearest(Ticks t, Ticks grid) { return (t + grid / 2) / grid * grid; }

/// A note and the notes tied on from it, in order.
std::vector<nt::ScoreNote> chain_of(const nt::Score& score, std::size_t staff, const nt::ScoreNote& head)
{
    std::vector<nt::ScoreNote> chain{head};
    const auto& notes = score.notes(staff);
    while (chain.back().tied_to_next) {
        const auto& cur = chain.back();
        const auto next = std::find_if(notes.begin(), notes.end(), [&](const nt::ScoreNote& n) {
            return !n.is_rest && !n.grace && n.voice == cur.voice && n.start_tick == cur.end_tick() &&
                   nt::midi_note_number(n.pitch) == nt::midi_note_number(cur.pitch);
        });
        if (next == notes.end()) break;
        chain.push_back(*next);
    }
    return chain;
}

/// The written values that fill [from, to), split at barlines and tied.
/// Empty when some piece cannot be written.
std::vector<std::pair<Ticks, nt::Duration>> write_span(const nt::Score& score, Ticks from, Ticks to, bool triplets)
{
    std::vector<std::pair<Ticks, nt::Duration>> out;
    Ticks t = from;
    while (t < to) {
        const auto m = score.measure_at(t);
        Ticks bar_end = m.start_tick + m.length_ticks;
        if (bar_end <= t) bar_end = to;
        const Ticks seg_end = std::min(to, bar_end);
        nt::Duration buf[16];
        std::size_t count = 0;
        if (triplets) {
            Ticks unwritten = 0;
            count = nt::notate_length(seg_end - t, buf, 16, unwritten);
            if (unwritten > 0) return {};
        } else {
            count = nt::decompose_ticks(seg_end - t, buf, 16);
        }
        if (count == 0) return {};
        Ticks at = t;
        for (std::size_t i = 0; i < count; ++i) {
            out.emplace_back(at, buf[i]);
            at += buf[i].ticks();
        }
        if (at != seg_end) return {};
        t = seg_end;
    }
    return out;
}

/// Writes a sounding span as a tie chain. `head` keeps its id, articulations
/// and voice unless `voice` says otherwise; returns false when the rhythm
/// cannot be written.
bool write_chain(nt::Score& score, std::size_t staff, nt::ScoreNote head, Ticks from, Ticks to, bool triplets,
                 bool keep_id)
{
    const auto pieces = write_span(score, from, to, triplets);
    if (pieces.empty()) return false;
    head.start_tick = from;
    head.duration = pieces.front().second;
    head.tied_to_next = pieces.size() > 1;
    if (keep_id) {
        if (!score.modify_note(head.id, head)) return false;
    } else if (score.insert_note(staff, head) == nt::kInvalidEventId) {
        return false;
    }
    for (std::size_t i = 1; i < pieces.size(); ++i) {
        nt::ScoreNote part = head;
        part.id = nt::kInvalidEventId;
        part.articulations = 0;
        part.start_tick = pieces[i].first;
        part.duration = pieces[i].second;
        part.tied_to_next = i + 1 < pieces.size();
        if (score.insert_note(staff, part) == nt::kInvalidEventId) return false;
    }
    return true;
}

/// Rests of `voice` under [from, to) go: a note now sounds there.
void clear_rests(nt::Score& score, std::size_t staff, std::uint8_t voice, Ticks from, Ticks to)
{
    std::vector<nt::EventId> rests;
    for (const auto& n : score.notes(staff))
        if (n.is_rest && n.voice == voice && n.start_tick < to && n.end_tick() > from) rests.push_back(n.id);
    for (auto id : rests) score.remove_note(id);
}

/// Marks notes sharing a start in a voice as chords, and lone notes as not.
void fix_chords(nt::Score& score, std::size_t staff)
{
    std::map<std::pair<Ticks, std::uint8_t>, int> count;
    for (const auto& n : score.notes(staff))
        if (!n.is_rest && !n.grace) ++count[{n.start_tick, n.voice}];
    std::vector<nt::ScoreNote> changed;
    for (const auto& n : score.notes(staff)) {
        if (n.is_rest || n.grace) continue;
        const bool chord = count[{n.start_tick, n.voice}] > 1;
        if (chord != n.in_chord) {
            nt::ScoreNote c = n;
            c.in_chord = chord;
            changed.push_back(c);
        }
    }
    for (const auto& c : changed) score.modify_note(c.id, c);
}

/// Notes of `voice` other than `exclude` that a span would collide with.
/// A note starting and ending with the span is a chord, not a collision.
std::vector<nt::ScoreNote> collisions(const nt::Score& score, std::size_t staff, std::uint8_t voice, Ticks from,
                                      Ticks to, const std::set<nt::EventId>& exclude)
{
    std::vector<nt::ScoreNote> out;
    for (const auto& n : score.notes(staff)) {
        if (n.is_rest || n.grace || n.voice != voice || exclude.count(n.id)) continue;
        if (n.start_tick >= to || n.end_tick() <= from) continue;
        if (n.start_tick == from && n.end_tick() == to) continue;
        out.push_back(n);
    }
    return out;
}

nt::KeySignature written_key_at(const nt::Score& score, std::size_t staff, Ticks tick)
{
    return interchange::written_key(score.key_signature_at(tick), score.staff(staff).transposition_semitones);
}

/// The other spelling of a chromatic note: the flat for a sharp and the
/// sharp for a flat (C# <-> Db). Empty for a natural.
std::optional<nt::Pitch> enharmonic(const nt::Pitch& p)
{
    if (p.alteration == 0) return std::nullopt;
    const int midi = nt::midi_note_number(p);
    for (int s = 0; s < 7; ++s) {
        nt::Pitch q;
        q.step = static_cast<nt::Step>(s);
        q.alteration = static_cast<std::int8_t>(p.alteration > 0 ? -1 : 1);
        for (int oct = p.octave - 1; oct <= p.octave + 1; ++oct) {
            q.octave = static_cast<std::int8_t>(oct);
            if (q.step != p.step && nt::midi_note_number(q) == midi) return q;
        }
    }
    return std::nullopt;
}

std::string pitch_name(const nt::Pitch& p)
{
    static const char* steps = "CDEFGAB";
    std::string s(1, steps[static_cast<int>(p.step)]);
    if (p.alteration > 0) s += std::string(static_cast<std::size_t>(p.alteration), '#');
    if (p.alteration < 0) s += std::string(static_cast<std::size_t>(-p.alteration), 'b');
    return s + std::to_string(p.octave);
}

std::string where(const nt::Score& score, Ticks t)
{
    const auto bb = score.to_bar_beat(t);
    char beat[16];
    std::snprintf(beat, sizeof beat, "%g", std::round(bb.beat * 100.0) / 100.0);
    return "bar " + std::to_string(bb.measure) + " beat " + beat;
}

const NoteEvent* rendered(const project::RenderedTrack& r, nt::EventId id)
{
    for (std::size_t i = 0; i < r.notes.size(); ++i)
        if (r.note_ids[i] == id) return &r.notes[i];
    return nullptr;
}

}  // namespace

std::pair<Ticks, Ticks> written_span(const nt::Score& score, nt::EventId note)
{
    const auto* n = score.find_note(note);
    if (!n) return {0, 0};
    const auto chain = chain_of(score, score.staff_index_of(note), *n);
    return {chain.front().start_tick, chain.back().end_tick()};
}

std::string describe(const NotateOption& option)
{
    return option.detail.empty() ? option.label : option.label + " - " + option.detail;
}

// --- A piano-roll edit of a score note ------------------------------------------------------

NotatePlan plan_note_edit(const project::Track& track, nt::EventId note, const NoteTarget& target)
{
    NotatePlan plan;
    const project::TrackVersion* v = track.active_version();
    if (!v) {
        plan.error = "The track has no version to edit";
        return plan;
    }
    const nt::Score& score = v->score;
    plan.base_revision = score.revision();
    const nt::ScoreNote* head = score.find_note(note);
    if (!head || head->is_rest || head->grace) {
        plan.error = "That note is no longer in the score";
        return plan;
    }
    const std::size_t staff = score.staff_index_of(note);
    const auto chain = chain_of(score, staff, *head);
    std::set<nt::EventId> chain_ids;
    for (const auto& c : chain) chain_ids.insert(c.id);

    const auto played = project::render_track(track);
    const NoteEvent* cur = rendered(played, note);
    if (!cur) {
        plan.error = "That note does not sound on its own (it continues a tie)";
        return plan;
    }
    const int transposition = score.staff(staff).total_transposition();
    const Ticks written_start = chain.front().start_tick;
    const Ticks written_end = chain.back().end_tick();
    const Ticks target_end = target.start + std::max<Ticks>(1, target.duration);
    // The edit's movement, carried over to the written span.
    const Ticks ws = std::max<Ticks>(0, written_start + (target.start - cur->start));
    const Ticks we = written_end + (target_end - cur->end());
    const int new_midi = std::clamp(target.pitch - transposition, 0, 127);
    const bool pitch_changed = new_midi != nt::midi_note_number(head->pitch);
    const bool timing_changed = ws != written_start || we != written_end;
    if (!pitch_changed && !timing_changed) {
        plan.error = "Nothing to change";
        return plan;
    }
    const nt::KeySignature key = written_key_at(score, staff, ws);
    const nt::Pitch spelled = pitch_changed ? nt::spell_midi_note(new_midi, key) : head->pitch;

    // A new pitch on the existing rhythm.
    auto with_pitch = [&](nt::Score s, const nt::Pitch& p) {
        for (const auto& c : chain) {
            nt::ScoreNote n = c;
            n.pitch = p;
            s.modify_note(n.id, n);
        }
        return s;
    };
    // The edit written into the score: the head keeps its id (and so any
    // performance it has); continuations are rewritten.
    auto notated = [&](Ticks from, Ticks to, bool triplets, std::uint8_t voice) -> std::optional<nt::Score> {
        nt::Score s = score;
        for (std::size_t i = 1; i < chain.size(); ++i) s.remove_note(chain[i].id);
        clear_rests(s, staff, voice, from, to);
        nt::ScoreNote h = *head;
        h.pitch = spelled;
        h.voice = voice;
        if (!write_chain(s, staff, h, from, to, triplets, true)) return std::nullopt;
        fix_chords(s, staff);
        return s;
    };
    auto rhythm_text = [&](const nt::Score& s, Ticks from, Ticks to, bool triplets) {
        const auto pieces = write_span(s, from, to, triplets);
        std::string text;
        for (std::size_t i = 0; i < pieces.size(); ++i) {
            const auto& d = pieces[i].second;
            if (i) text += " tied to ";
            text += std::string(d.dots ? "dotted " : "") + nt::to_string(d.value) + (d.tuplet.is_tuplet() ? " triplet" : "");
        }
        return text + " at " + where(s, from);
    };

    NotateOption override_option;
    override_option.id = "override";
    override_option.label = "Keep it as a performance";
    override_option.detail = "the score stays as written; only how the note is played changes";
    override_option.score = pitch_changed ? with_pitch(score, spelled) : score;
    override_option.performance = v->performance;
    {
        project::NoteOverride o;
        if (auto it = v->performance.find(note); it != v->performance.end()) o = it->second;
        o.onset += target.start - cur->start;
        o.length += std::max<Ticks>(1, target.duration) - cur->duration;
        const auto clamped = project::clamp_override(o);
        if (clamped != o) override_option.detail += " (as far as a performance can move a note)";
        if (clamped.empty())
            override_option.performance.erase(note);
        else
            override_option.performance[note] = clamped;
    }
    override_option.changes_notation = pitch_changed;

    if (!timing_changed) {
        // A pitch edit is always notation; a chromatic note can be spelled two ways.
        NotateOption o;
        o.id = "notate";
        o.label = "Write " + pitch_name(spelled);
        o.score = with_pitch(score, spelled);
        o.performance = v->performance;
        o.changes_notation = true;
        plan.options.push_back(std::move(o));
        if (const auto other = enharmonic(spelled); other && spelled.alteration != key.alteration_for(spelled.step)) {
            plan.ambiguity = "spelling";
            plan.question = "Spell the new note as " + pitch_name(spelled) + " or " + pitch_name(*other) + "?";
            NotateOption alt;
            alt.id = spelled.alteration > 0 ? "flat" : "sharp";
            alt.label = "Write " + pitch_name(*other);
            alt.score = with_pitch(score, *other);
            alt.performance = v->performance;
            alt.changes_notation = true;
            plan.options.push_back(std::move(alt));
        }
        return plan;
    }

    if (we <= ws || (!(on_straight(ws) && on_straight(we)) && !(on_triplet(ws) && on_triplet(we)))) {
        // Off every grid notation has: a performance.
        plan.options.push_back(std::move(override_option));
        return plan;
    }

    struct Rhythm {
        Ticks from, to;
        bool triplets;
        std::string id, label;
    };
    std::vector<Rhythm> rhythms;
    const bool straight = on_straight(ws) && on_straight(we);
    if (straight) {
        rhythms.push_back({ws, we, false, "notate", "Write it in the score"});
    } else {
        plan.ambiguity = "tuplet";
        plan.question = "The edit lands on a triplet. Write a triplet, round it to straight time, or keep it as a performance?";
        rhythms.push_back({ws, we, true, "triplet", "Write it as a triplet"});
        const Ticks sf = nearest(ws, kStraight), st = nearest(we, kStraight);
        if (st > sf) rhythms.push_back({sf, st, false, "straight", "Round it to straight time"});
    }

    bool collided = false;
    for (const auto& r : rhythms) {
        if (collisions(score, staff, head->voice, r.from, r.to, chain_ids).empty()) {
            if (auto s = notated(r.from, r.to, r.triplets, head->voice)) {
                NotateOption o;
                o.id = r.id;
                o.label = r.label;
                o.detail = rhythm_text(*s, r.from, r.to, r.triplets);
                o.score = std::move(*s);
                o.performance = v->performance;
                o.changes_notation = true;
                plan.options.push_back(std::move(o));
            }
            continue;
        }
        collided = true;
        // Another line of the same staff: the first voice that is free there.
        for (std::uint8_t voice = 0; voice < 4; ++voice) {
            if (voice == head->voice || !collisions(score, staff, voice, r.from, r.to, chain_ids).empty()) continue;
            if (auto s = notated(r.from, r.to, r.triplets, voice)) {
                NotateOption o;
                o.id = r.id == "notate" ? "voice" : r.id + "-voice";
                o.label = r.label + " in voice " + std::to_string(voice + 1);
                o.detail = rhythm_text(*s, r.from, r.to, r.triplets) + ", as a line of its own";
                o.score = std::move(*s);
                o.performance = v->performance;
                o.changes_notation = true;
                plan.options.push_back(std::move(o));
            }
            break;
        }
    }
    if (collided && plan.ambiguity.empty()) {
        plan.ambiguity = "overlap";
        plan.question = "The note now overlaps another of its line. Write it in another voice, or keep it as a performance?";
    }
    // Clean notation: nothing to ask.
    if (!collided && plan.ambiguity.empty() && !plan.options.empty()) return plan;
    // Stretching a note into the next is usually legato: the performance reading first.
    if (collided && ws == written_start)
        plan.options.insert(plan.options.begin(), override_option);
    else
        plan.options.push_back(override_option);
    if (plan.options.size() == 1) {
        plan.ambiguity.clear();
        plan.question.clear();
    }
    return plan;
}

// --- Notating a MIDI region -------------------------------------------------------------------

NotatePlan plan_notate_region(const project::Track& track, const std::string& region_id,
                              const RegionNotateSettings& settings)
{
    NotatePlan plan;
    const project::TrackVersion* v = track.active_version();
    if (!v) {
        plan.error = "The track has no version to notate into";
        return plan;
    }
    const auto region = std::find_if(v->regions.begin(), v->regions.end(),
                                     [&](const project::MidiRegion& r) { return r.id == region_id; });
    if (region == v->regions.end() || region->notes.empty()) {
        plan.error = "The region has no notes to notate";
        return plan;
    }
    const nt::Score& score = v->score;
    plan.base_revision = score.revision();
    if (score.staff_count() == 0) {
        plan.error = "The track's score has no staff";
        return plan;
    }
    constexpr std::size_t staff = 0;
    const int transposition = score.staff(staff).total_transposition();
    const Ticks grid = nt::grid_ticks(settings.grid);
    if (grid <= 0) {
        plan.error = "Unknown grid";
        return plan;
    }

    struct Sound {
        Ticks start, end;
        nt::Pitch pitch;
        std::uint8_t voice;
        bool triplet;
    };
    // The region read as notation, through the notation engine's own MIDI
    // reader: quantized, voices told apart, spelled in the written key.
    auto read = [&](bool triplets, std::vector<Sound>& sounds, Ticks& largest_move) -> bool {
        music::SmfFile file;
        file.ticks_per_quarter = static_cast<std::uint16_t>(kTicksPerQuarter);
        music::SmfTrack t;
        for (const auto& n : region->notes) {
            const int midi = n.pitch - transposition;
            if (midi < 0 || midi > 127) continue;
            const auto key = static_cast<std::uint8_t>(midi);
            t.events.push_back({n.start, music::make_note_on(0, key, static_cast<std::uint8_t>(std::clamp(n.velocity, 1, 127)))});
            t.events.push_back({n.end(), music::make_note_off(0, key)});
        }
        std::stable_sort(t.events.begin(), t.events.end(),
                         [](const music::SmfEvent& a, const music::SmfEvent& b) { return a.tick < b.tick; });
        file.tracks.push_back(std::move(t));
        nt::MidiImportOptions o;
        o.grid = settings.grid;
        o.allow_triplets = triplets;
        o.read_dynamics = false;
        o.read_markers = false;
        o.guess_clefs = false;
        o.default_key = written_key_at(score, staff, region->start);
        nt::Score tmp;
        const auto r = nt::import_midi(file, tmp, o);
        if (!r.ok || tmp.staff_count() == 0) return false;
        largest_move = r.largest_move;
        std::set<nt::EventId> seen;
        for (const auto& n : tmp.notes(0)) {
            if (n.is_rest || n.grace || seen.count(n.id)) continue;
            const auto chain = chain_of(tmp, 0, n);
            for (const auto& c : chain) seen.insert(c.id);
            Sound s{chain.front().start_tick, chain.back().end_tick(), n.pitch, n.voice, false};
            s.triplet = !on_straight(s.start) || !on_straight(s.end);
            sounds.push_back(s);
        }
        return !sounds.empty();
    };

    std::vector<Sound> sounds;
    Ticks largest_move = 0;
    if (!read(settings.allow_triplets, sounds, largest_move)) {
        plan.error = "Nothing in the region could be written as notes";
        return plan;
    }
    const bool heard_triplets = std::any_of(sounds.begin(), sounds.end(), [](const Sound& s) { return s.triplet; });
    Ticks from = sounds.front().start, to = 0;
    std::uint8_t voices_used = 0;
    for (const auto& s : sounds) {
        from = std::min(from, s.start);
        to = std::max(to, s.end);
        voices_used = std::max<std::uint8_t>(voices_used, static_cast<std::uint8_t>(s.voice + 1));
    }

    // What is already written where the region lands.
    auto written_in = [&](const nt::Score& s, std::uint8_t voice) {
        for (const auto& n : s.notes(staff))
            if (!n.is_rest && !n.grace && n.voice == voice && n.start_tick < to && n.end_tick() > from) return true;
        return false;
    };
    bool lands_on_notes = false;
    for (std::uint8_t vc = 0; vc < voices_used; ++vc) lands_on_notes = lands_on_notes || written_in(score, vc);

    auto build = [&](const std::vector<Sound>& snds, bool triplets, int voice_offset, bool replace,
                     NotateOption& o) -> bool {
        nt::Score s = score;
        if (replace) {
            std::vector<nt::EventId> gone;
            for (const auto& n : s.notes(staff))
                if (!n.grace && n.start_tick < to && n.end_tick() > from) gone.push_back(n.id);
            for (auto id : gone) s.remove_note(id);
        }
        std::vector<nt::EventId> heads;
        for (const auto& snd : snds) {
            const auto voice = static_cast<std::uint8_t>(snd.voice + voice_offset);
            if (voice > 3) return false;
            clear_rests(s, staff, voice, snd.start, snd.end);
            nt::ScoreNote h;
            h.pitch = snd.pitch;
            h.voice = voice;
            const auto before = s.total_note_count();
            if (!write_chain(s, staff, h, snd.start, snd.end, snd.triplet, false)) return false;
            // The head is the note inserted first: the one at the chain's start.
            for (const auto& n : s.notes(staff))
                if (n.start_tick == snd.start && n.voice == voice && n.pitch == snd.pitch && !n.is_rest &&
                    std::find(heads.begin(), heads.end(), n.id) == heads.end() && s.total_note_count() > before) {
                    heads.push_back(n.id);
                    break;
                }
        }
        fix_chords(s, staff);
        o.score = std::move(s);
        o.performance = v->performance;
        o.removed_regions = {region_id};
        o.changes_notation = true;
        if (!settings.keep_feel) return true;

        // Keep the played feel: each played note against what the new
        // notation renders, as an override of its head.
        project::Track probe = track;
        auto* pv = probe.active_version();
        pv->score = o.score;
        pv->regions.erase(std::remove_if(pv->regions.begin(), pv->regions.end(),
                                         [&](const project::MidiRegion& r) { return r.id == region_id; }),
                          pv->regions.end());
        const auto as_written = project::render_track(probe, false);
        std::set<nt::EventId> used;
        for (const auto& p : region->notes) {
            const Ticks q = nt::quantize_tick(p.start, grid, triplets);
            for (std::size_t i = 0; i < as_written.notes.size(); ++i) {
                const auto id = as_written.note_ids[i];
                if (id == nt::kInvalidEventId || used.count(id) ||
                    std::find(heads.begin(), heads.end(), id) == heads.end())
                    continue;
                const auto* written = o.score.find_note(id);
                if (!written || written->start_tick != q || as_written.notes[i].pitch != p.pitch) continue;
                used.insert(id);
                project::NoteOverride ov;
                ov.onset = p.start - as_written.notes[i].start;
                ov.length = p.duration - as_written.notes[i].duration;
                ov.velocity = p.velocity - as_written.notes[i].velocity;
                ov = project::clamp_override(ov);
                if (!ov.empty()) o.performance[id] = ov;
                break;
            }
        }
        return true;
    };

    const auto count_text = std::to_string(sounds.size()) + (sounds.size() == 1 ? " note" : " notes");
    const std::string grid_text = std::string("quantized to ") + nt::to_string(settings.grid) +
                                  (settings.allow_triplets ? " (and triplets)" : "") +
                                  (largest_move > 0 ? ", moved up to " + std::to_string(largest_move) + " ticks" : "");
    const std::string feel = settings.keep_feel ? "; it still plays as recorded" : "; it plays as written";

    auto add = [&](const std::string& id, const std::string& label, const std::string& detail,
                   const std::vector<Sound>& snds, bool triplets, int offset, bool replace) {
        NotateOption o;
        o.id = id;
        o.label = label;
        o.detail = detail;
        if (build(snds, triplets, offset, replace, o)) plan.options.push_back(std::move(o));
    };

    if (!lands_on_notes) {
        add("notate", "Notate the region", count_text + ", " + grid_text + feel, sounds, settings.allow_triplets, 0, false);
    } else {
        plan.ambiguity = "placement";
        plan.question = "The region lands on notes already written. Write it as another voice, or replace what is written there?";
        int offset = -1;
        for (int k = 1; k + voices_used <= 4 && offset < 0; ++k) {
            bool free = true;
            for (int vc = 0; vc < voices_used; ++vc) free = free && !written_in(score, static_cast<std::uint8_t>(vc + k));
            if (free) offset = k;
        }
        if (offset > 0)
            add("voice", "Write it in voice " + std::to_string(offset + 1),
                count_text + " under the written line, " + grid_text + feel, sounds, settings.allow_triplets, offset, false);
        add("replace", "Replace what is written there",
            "the written notes under the region go; " + count_text + ", " + grid_text + feel, sounds,
            settings.allow_triplets, 0, true);
    }
    if (heard_triplets && !plan.options.empty()) {
        std::vector<Sound> straight;
        Ticks move = 0;
        if (read(false, straight, move)) {
            const auto& first = plan.options.front();
            const int offset = first.id == "voice" ? [&] {
                for (int k = 1; k < 4; ++k) {
                    bool free = true;
                    for (int vc = 0; vc < voices_used; ++vc)
                        free = free && !written_in(score, static_cast<std::uint8_t>(vc + k));
                    if (free) return k;
                }
                return 0;
            }()
                                                         : 0;
            const bool replace = first.id == "replace";
            add("straight", first.label + ", in straight time",
                "no triplets: " + std::to_string(straight.size()) + " notes" + feel, straight, false, offset, replace);
            if (plan.ambiguity.empty()) {
                plan.ambiguity = "tuplet";
                plan.question = "The region has notes on a triplet grid. Write triplets, or straight time?";
            }
        }
    }
    if (plan.options.empty()) plan.error = "The region could not be written as notes";
    return plan;
}

}  // namespace vcc::session
