#include "vcc/project/Render.h"

#include <algorithm>
#include <cmath>
#include <tuple>
#include <deque>
#include <map>

#include "vcc/notation/MidiRenderer.h"
#include "vcc/project/ArticulationMap.h"
#include "vcc/project/Percussion.h"

namespace vcc::project {

notation::ExpressionMap expression_map_named(const std::string& name)
{
    if (name == "orchestral_cc") return notation::make_orchestral_cc_expression_map();
    return notation::make_default_expression_map();
}

RenderedTrack render_track(const Track& track, bool performance)
{
    return render_track(track, expression_map_for(track), performance);
}

NoteEvent apply_override(NoteEvent note, const NoteOverride& o) noexcept
{
    note.start = std::max<Ticks>(0, note.start + o.onset);
    note.duration = std::max<Ticks>(1, note.duration + o.length);
    note.velocity = std::clamp(note.velocity + o.velocity, 1, 127);
    note.pitch = std::clamp(note.pitch + o.pitch, 0, 127);
    return note;
}

namespace {
int send_priority(const music::MidiEvent& event) noexcept
{
    switch (event.type) {
        case music::MidiEventType::NoteOff: return 0;
        case music::MidiEventType::ControlChange:
        case music::MidiEventType::ProgramChange:
        case music::MidiEventType::PitchBend: return 1;
        case music::MidiEventType::NoteOn: return 3;
        default: return 2;
    }
}
}  // namespace

void canonical_order(std::vector<notation::TimedMidiEvent>& events)
{
    std::sort(events.begin(), events.end(), [](const notation::TimedMidiEvent& a, const notation::TimedMidiEvent& b) {
        if (a.tick != b.tick) return a.tick < b.tick;
        const int pa = send_priority(a.event), pb = send_priority(b.event);
        if (pa != pb) return pa < pb;
        if (a.source_id != b.source_id) return a.source_id < b.source_id;
        if (a.role != b.role) return a.role < b.role;
        if (a.event.type != b.event.type) return a.event.type < b.event.type;
        if (a.event.channel != b.event.channel) return a.event.channel < b.event.channel;
        if (a.event.data1 != b.event.data1) return a.event.data1 < b.event.data1;
        return a.event.data2 < b.event.data2;
    });
}

notation::RenderResult render_score_events(const Track& track, const notation::ExpressionMap& map, Ticks from,
                                           Ticks to)
{
    const TrackVersion* version = track.active_version();
    if (!version) return {};
    notation::MidiRenderer renderer(map);
    notation::RenderOptions options;
    options.channel = static_cast<std::uint8_t>(std::clamp(track.midi_channel, 0, 15));
    options.from_tick = from;
    options.to_tick = to;
    renderer.set_options(options);
    notation::RenderResult r = renderer.render(version->score);
    // A drum kit's staff positions play their drums (Main Spec §7).
    if (const PercussionMap* drums = track.percussion_map.empty() ? nullptr : percussion_map_named(track.percussion_map))
        for (auto& e : r.events)
            if (e.role == notation::EventRole::Note &&
                (e.event.type == music::MidiEventType::NoteOn || e.event.type == music::MidiEventType::NoteOff))
                if (const auto* sound = drums->by_written(e.event.data1)) e.event.data1 = static_cast<std::uint8_t>(sound->midi);
    canonical_order(r.events);
    std::sort(r.unmapped_articulations.begin(), r.unmapped_articulations.end());
    std::sort(r.out_of_range_notes.begin(), r.out_of_range_notes.end());
    return r;
}

RenderedTrack render_track(const Track& track, const notation::ExpressionMap& map, bool performance)
{
    return finish_render(track, map, render_score_events(track, map), performance);
}

namespace {
std::vector<Ticks> notes_starts(const std::vector<std::pair<NoteEvent, notation::EventId>>& notes)
{
    std::vector<Ticks> out;
    out.reserve(notes.size());
    for (const auto& n : notes) out.push_back(n.first.start);
    std::sort(out.begin(), out.end());
    return out;
}
}  // namespace

void add_hairpin_expression(const notation::Score& score, int channel, const std::vector<Ticks>& note_starts,
                            std::vector<ControllerEvent>& out)
{
    const auto& hairpins = score.hairpins();
    if (hairpins.empty()) return;
    std::vector<notation::Hairpin> sorted(hairpins.begin(), hairpins.end());
    std::sort(sorted.begin(), sorted.end(),
              [](const notation::Hairpin& a, const notation::Hairpin& b) {
                  return std::tie(a.start_tick, a.end_tick, a.staff) < std::tie(b.start_tick, b.end_tick, b.staff);
              });
    // The same hairpin written twice plays once.
    sorted.erase(std::unique(sorted.begin(), sorted.end(),
                             [](const notation::Hairpin& a, const notation::Hairpin& b) {
                                 return a.start_tick == b.start_tick && a.end_tick == b.end_tick && a.kind == b.kind &&
                                        a.expression_start == b.expression_start && a.expression_end == b.expression_end;
                             }),
                 sorted.end());
    constexpr int kCc = 11;
    out.push_back(ControllerEvent{0, channel, kCc, notation::kNeutralExpression});
    const Ticks step = std::max<Ticks>(1, notation::kTicksPerQuarter / 8);  // a 32nd: smooth to the ear
    for (std::size_t i = 0; i < sorted.size(); ++i) {
        const auto& h = sorted[i];
        const int v0 = notation::hairpin_expression_start(h), v1 = notation::hairpin_expression_end(h);
        const Ticks span = std::max<Ticks>(1, h.end_tick - h.start_tick);
        int last = -1;
        for (Ticks t = h.start_tick; t < h.end_tick; t += step) {
            const double f = notation::evaluate_curve(h.shape, static_cast<double>(t - h.start_tick) / static_cast<double>(span));
            const int v = std::clamp(static_cast<int>(std::lround(v0 + (v1 - v0) * f)), 0, 127);
            if (v != last) out.push_back(ControllerEvent{t, channel, kCc, v});
            last = v;
        }
        out.push_back(ControllerEvent{h.end_tick, channel, kCc, v1});
        // Back to the neutral level at the next note after it (unless the
        // next hairpin begins first, or it already ended there).
        if (v1 != notation::kNeutralExpression) {
            const auto next = std::lower_bound(note_starts.begin(), note_starts.end(), h.end_tick);
            if (next != note_starts.end() && (i + 1 >= sorted.size() || *next < sorted[i + 1].start_tick))
                out.push_back(ControllerEvent{*next, channel, kCc, notation::kNeutralExpression});
        }
    }
}

RenderedTrack finish_render(const Track& track, const notation::ExpressionMap& map, const notation::RenderResult& r,
                            bool performance)
{
    RenderedTrack out;
    const TrackVersion* version = track.active_version();
    if (!version) return out;
    const int channel = std::clamp(track.midi_channel, 0, 15);

    auto override_for = [&](notation::EventId id) -> const NoteOverride* {
        if (!performance || id == notation::kInvalidEventId) return nullptr;
        const auto it = version->performance.find(id);
        return it == version->performance.end() || it->second.empty() ? nullptr : &it->second;
    };

    // Pair note-ons with note-offs per (channel, pitch), first in first out.
    // A FIFO per (channel, pitch): a vector and a read position, which
    // costs nothing until a pitch is used.
    std::vector<std::vector<const notation::TimedMidiEvent*>> open(16 * 128);
    std::vector<std::size_t> open_head(16 * 128, 0);
    std::vector<std::pair<NoteEvent, notation::EventId>> notes;
    // Notes whose override moves the dynamics controller: (performed note, offset).
    std::vector<std::pair<NoteEvent, int>> cc_spans;
    for (const auto& e : r.events) {
        const auto& m = e.event;
        const int pitch = m.data1;
        const auto key = static_cast<std::size_t>((m.channel & 15) * 128 + (pitch & 127));
        const bool on = m.type == music::MidiEventType::NoteOn && m.data2 > 0;
        const bool off = m.type == music::MidiEventType::NoteOff ||
                         (m.type == music::MidiEventType::NoteOn && m.data2 == 0);
        const NoteOverride* o = override_for(e.source_id);
        if (on) {
            open[key].push_back(&e);
        } else if (off) {
            auto& q = open[key];
            if (open_head[key] >= q.size()) continue;
            const auto* start = q[open_head[key]++];
            NoteEvent n{start->tick, std::max<Ticks>(1, e.tick - start->tick), pitch, start->event.data2,
                        static_cast<int>(m.channel), {}};
            const NoteOverride* so = override_for(start->source_id);
            if (start->role == notation::EventRole::Keyswitch) {
                // A keyswitch travels with its note, so it still leads it.
                if (so) n.start = std::max<Ticks>(0, n.start + so->onset);
                n.articulation = "keyswitch";
                out.keyswitches.push_back(n);
            } else {
                if (so) {
                    n = apply_override(n, *so);
                    if (so->cc != 0) cc_spans.emplace_back(n, so->cc);
                }
                notes.emplace_back(n, start->source_id);
            }
        } else if (m.type == music::MidiEventType::ProgramChange) {
            // An articulation map's program switch: it leads its note.
            ControllerEvent c{e.tick, static_cast<int>(m.channel), kProgramChangeController, m.data1};
            if (o) c.tick = std::max<Ticks>(0, c.tick + o->onset);
            out.controllers.push_back(c);
        } else if (m.type == music::MidiEventType::ControlChange) {
            ControllerEvent c{e.tick, static_cast<int>(m.channel), m.data1, m.data2};
            if (o) {  // a controller the note itself sends (an articulation CC)
                c.tick = std::max<Ticks>(0, c.tick + o->onset);
                c.value = std::clamp(c.value + o->cc, 0, 127);
            }
            out.controllers.push_back(c);
        }
    }
    if (!r.out_of_range_notes.empty())
        out.warnings.push_back(std::to_string(r.out_of_range_notes.size()) +
                               " note(s) fall outside MIDI range after transposition");
    for (auto a : r.unmapped_articulations)
        out.warnings.push_back(std::string("no mapping for articulation '") + notation::to_string(a) +
                               "' in this expression map");

    // Crescendos and diminuendos swell what is sounding: each hairpin is a
    // ramp of expression (CC11) from its start value to its end value, so a
    // held chord grows or dies away, not only the next note's attack. A part
    // with hairpins plays at the neutral expression elsewhere; after one that
    // ends away from it, the next note brings it back.
    add_hairpin_expression(version->score, channel, notes_starts(notes), out.controllers);

    std::stable_sort(out.controllers.begin(), out.controllers.end(),
                     [](const ControllerEvent& a, const ControllerEvent& b) { return a.tick < b.tick; });

    // A CC offset on a map that shapes dynamics with a controller: lift (or
    // drop) that controller for the note's span, then put back what the
    // score asks for at the note's end.
    if (!cc_spans.empty() && map.dynamic_target != notation::DynamicTarget::Velocity) {
        const int cc = map.dynamics_cc;
        const std::vector<ControllerEvent> written = out.controllers;
        auto value_at = [&](Ticks t) {
            int v = -1;
            for (const auto& c : written) {
                if (c.tick > t) break;
                if (c.controller == cc) v = c.value;
            }
            return v;
        };
        for (const auto& [n, offset] : cc_spans) {
            const int base = value_at(n.start);
            const int restore = value_at(n.end());
            const int from = base < 0 ? 64 : base;
            out.controllers.push_back({n.start, channel, cc, std::clamp(from + offset, 0, 127)});
            out.controllers.push_back({n.end(), channel, cc, restore < 0 ? from : restore});
        }
        std::stable_sort(out.controllers.begin(), out.controllers.end(),
                         [](const ControllerEvent& a, const ControllerEvent& b) { return a.tick < b.tick; });
    }

    bool region_cc = false;
    for (const auto& region : version->regions) {
        if (region.lane != 0) continue;  // take lanes are silent
        for (auto n : region.notes) {
            n.channel = channel;
            notes.emplace_back(n, notation::kInvalidEventId);
        }
        for (auto c : region.controllers) {
            c.channel = channel;
            out.controllers.push_back(c);
            region_cc = true;
        }
    }
    if (region_cc)
        std::stable_sort(out.controllers.begin(), out.controllers.end(),
                         [](const ControllerEvent& a, const ControllerEvent& b) { return a.tick < b.tick; });
    std::stable_sort(notes.begin(), notes.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    out.notes.reserve(notes.size());
    out.note_ids.reserve(notes.size());
    for (auto& [n, id] : notes) {
        out.notes.push_back(std::move(n));
        out.note_ids.push_back(id);
    }
    std::sort(out.keyswitches.begin(), out.keyswitches.end());
    // Track Trans: the shift heard, once, on every note (keyswitches are
    // instructions to the sampler, not pitches, and stay where they are).
    if (performance && track.effective_transpose() != 0) {
        const int by = track.effective_transpose();
        int dropped = 0;
        for (auto& n : out.notes) {
            const int p = n.pitch + by;
            if (p < 0 || p > 127) ++dropped;
            n.pitch = std::clamp(p, 0, 127);
        }
        if (dropped > 0) out.warnings.push_back(std::to_string(dropped) + " note(s) pinned at the MIDI range by the track's transpose");
    }
    return out;
}

std::string export_project_midi(const Project& project, bool include_keyswitches)
{
    std::vector<MidiFileTrack> tracks;
    for (const Track* t : project.export_tracks()) {
        if (!t->articulation_parent.empty()) continue;  // its notes are its parent's
        if (t->kind != TrackKind::Instrument && t->kind != TrackKind::MidiOnly) continue;
        RenderedTrack r = render_track(*t);
        MidiFileTrack mt{t->name, r.notes, t->midi_channel == 9 ? -1 : t->assignment.placeholder_program, r.controllers};
        if (include_keyswitches) mt.notes.insert(mt.notes.end(), r.keyswitches.begin(), r.keyswitches.end());
        tracks.push_back(std::move(mt));
    }
    MidiFileOptions o;
    o.meter = project.data().meter;
    o.tempo = project.data().tempo;
    for (const auto& cue : project.data().cues) o.markers.emplace_back(cue.start, cue.code + " " + cue.title);
    return write_midi_file(tracks, o);
}

}  // namespace vcc::project
