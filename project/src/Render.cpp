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

RenderedTrack render_track(const Track& track) { return render_track(track, expression_map_named(track.expression_map)); }

RenderedTrack render_track(const Track& track, const notation::ExpressionMap& map)
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

    // Pair note-ons with note-offs per (channel, pitch), first in first out.
    std::map<std::pair<int, int>, std::deque<std::pair<const notation::TimedMidiEvent*, Ticks>>> open;
    for (const auto& e : r.events) {
        const auto& m = e.event;
        const int pitch = m.data1;
        const auto key = std::make_pair(static_cast<int>(m.channel), pitch);
        const bool on = m.type == music::MidiEventType::NoteOn && m.data2 > 0;
        const bool off = m.type == music::MidiEventType::NoteOff ||
                         (m.type == music::MidiEventType::NoteOn && m.data2 == 0);
        if (on) {
            open[key].emplace_back(&e, e.tick);
        } else if (off) {
            auto& q = open[key];
            if (q.empty()) continue;
            const auto* start = q.front().first;
            q.pop_front();
            NoteEvent n{start->tick, std::max<Ticks>(1, e.tick - start->tick), pitch, start->event.data2,
                        static_cast<int>(m.channel), {}};
            if (start->role == notation::EventRole::Keyswitch) {
                n.articulation = "keyswitch";
                out.keyswitches.push_back(n);
            } else {
                out.notes.push_back(n);
            }
        } else if (m.type == music::MidiEventType::ControlChange) {
            out.controllers.push_back({e.tick, static_cast<int>(m.channel), m.data1, m.data2});
        }
    }
    if (!r.out_of_range_notes.empty())
        out.warnings.push_back(std::to_string(r.out_of_range_notes.size()) +
                               " note(s) fall outside MIDI range after transposition");
    for (auto a : r.unmapped_articulations)
        out.warnings.push_back(std::string("no mapping for articulation '") + notation::to_string(a) +
                               "' in this expression map");

    for (const auto& region : version->regions)
        for (auto n : region.notes) {
            n.channel = channel;
            out.notes.push_back(n);
        }
    std::sort(out.notes.begin(), out.notes.end());
    std::sort(out.keyswitches.begin(), out.keyswitches.end());
    std::stable_sort(out.controllers.begin(), out.controllers.end(),
                     [](const ControllerEvent& a, const ControllerEvent& b) { return a.tick < b.tick; });
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
