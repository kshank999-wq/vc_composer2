#include "vcc/project/Render.h"

#include <algorithm>
#include <deque>
#include <map>

#include "vcc/notation/MidiRenderer.h"

namespace vcc::project {

notation::ExpressionMap expression_map_named(const std::string& name)
{
    if (name == "orchestral_cc") return notation::make_orchestral_cc_expression_map();
    return notation::make_default_expression_map();
}

RenderedTrack render_track(const Track& track, bool performance)
{
    return render_track(track, expression_map_named(track.expression_map), performance);
}

NoteEvent apply_override(NoteEvent note, const NoteOverride& o) noexcept
{
    note.start = std::max<Ticks>(0, note.start + o.onset);
    note.duration = std::max<Ticks>(1, note.duration + o.length);
    note.velocity = std::clamp(note.velocity + o.velocity, 1, 127);
    return note;
}

RenderedTrack render_track(const Track& track, const notation::ExpressionMap& map, bool performance)
{
    RenderedTrack out;
    const TrackVersion* version = track.active_version();
    if (!version) return out;
    const int channel = std::clamp(track.midi_channel, 0, 15);

    notation::MidiRenderer renderer(map);
    notation::RenderOptions options;
    options.channel = static_cast<std::uint8_t>(channel);
    renderer.set_options(options);
    notation::RenderResult r = renderer.render(version->score);

    auto override_for = [&](notation::EventId id) -> const NoteOverride* {
        if (!performance || id == notation::kInvalidEventId) return nullptr;
        const auto it = version->performance.find(id);
        return it == version->performance.end() || it->second.empty() ? nullptr : &it->second;
    };

    // Pair note-ons with note-offs per (channel, pitch), first in first out.
    std::map<std::pair<int, int>, std::deque<std::pair<const notation::TimedMidiEvent*, Ticks>>> open;
    std::vector<std::pair<NoteEvent, notation::EventId>> notes;
    // Notes whose override moves the dynamics controller: (performed note, offset).
    std::vector<std::pair<NoteEvent, int>> cc_spans;
    for (const auto& e : r.events) {
        const auto& m = e.event;
        const int pitch = m.data1;
        const auto key = std::make_pair(static_cast<int>(m.channel), pitch);
        const bool on = m.type == music::MidiEventType::NoteOn && m.data2 > 0;
        const bool off = m.type == music::MidiEventType::NoteOff ||
                         (m.type == music::MidiEventType::NoteOn && m.data2 == 0);
        const NoteOverride* o = override_for(e.source_id);
        if (on) {
            open[key].emplace_back(&e, e.tick);
        } else if (off) {
            auto& q = open[key];
            if (q.empty()) continue;
            const auto* start = q.front().first;
            q.pop_front();
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

    for (const auto& region : version->regions)
        for (auto n : region.notes) {
            n.channel = channel;
            notes.emplace_back(n, notation::kInvalidEventId);
        }
    std::stable_sort(notes.begin(), notes.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    out.notes.reserve(notes.size());
    out.note_ids.reserve(notes.size());
    for (auto& [n, id] : notes) {
        out.notes.push_back(std::move(n));
        out.note_ids.push_back(id);
    }
    std::sort(out.keyswitches.begin(), out.keyswitches.end());
    return out;
}

std::string export_project_midi(const Project& project, bool include_keyswitches)
{
    std::vector<MidiFileTrack> tracks;
    for (const Track* t : project.export_tracks()) {
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
