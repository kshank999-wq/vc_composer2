// Importing a Standard MIDI File into regions (drag-and-drop, File menu).
#include "vcc/session/Session.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <map>

#include "vcc/music/MidiFile.h"
#include "vcc/templates/Templates.h"

namespace vcc::session {

namespace {

struct Part {
    std::string name;
    int channel = 0;
    int program = -1;
    std::vector<NoteEvent> notes;
    std::vector<ControllerEvent> controllers;
};

/// The catalog instrument to give a new track for a part.
std::string part_instrument(const templates::InstrumentRegistry& catalog, const Part& p)
{
    const auto& all = catalog.all();
    if (p.channel == 9)
        for (const auto& def : all)
            if (def.unpitched) return def.id;
    if (p.program >= 0)
        for (const auto& def : all)
            if (!def.unpitched && def.gm_program == p.program) return def.id;
    if (catalog.find("keys.piano")) return "keys.piano";
    for (const auto& def : all)
        if (!def.unpitched) return def.id;
    return {};
}

}  // namespace

Session::MidiImport Session::import_midi_file(const std::string& path, const std::string& target_track, Ticks at)
{
    MidiImport result;
    music::SmfFile file;
    const auto read = music::read_smf_file(path, file);
    if (!read.ok) {
        result.error = read.error.empty() ? std::string("not a MIDI file") : read.error;
        return result;
    }
    const std::int64_t tpq = std::max<std::int64_t>(1, file.ticks_per_quarter);
    auto scale = [&](std::int64_t t) {
        return static_cast<Ticks>((t * kTicksPerQuarter + tpq / 2) / tpq);
    };
    const std::string stem = std::filesystem::path(path).stem().string();

    // Parts: a file track per channel it uses.
    double bpm = 0.0;
    TimeSignature sig;
    bool have_sig = false;
    std::vector<Part> parts;
    for (std::size_t ti = 0; ti < file.tracks.size(); ++ti) {
        const auto& track = file.tracks[ti];
        for (const auto& m : track.meta) {
            if (m.tick != 0) continue;
            if (m.type == music::smf_meta::kTempo && m.data.size() == 3 && bpm == 0.0) {
                const double us = static_cast<double>((m.data[0] << 16) | (m.data[1] << 8) | m.data[2]);
                if (us > 0) bpm = 60.0e6 / us;
            }
            if (m.type == music::smf_meta::kTimeSignature && m.data.size() >= 2 && !have_sig) {
                sig.numerator = m.data[0];
                sig.denominator = 1 << std::min<int>(m.data[1], 5);
                have_sig = sig.is_valid();
            }
        }
        std::map<int, Part> by_channel;
        std::map<int, std::vector<std::pair<Ticks, int>>> open;  // channel*128+pitch -> starts, velocities
        for (const auto& e : track.events) {
            const int ch = e.event.channel & 15;
            Part& part = by_channel[ch];
            part.channel = ch;
            const Ticks t = scale(e.tick);
            switch (e.event.type) {
                case music::MidiEventType::NoteOn:
                    if (e.event.data2 > 0) {
                        open[ch * 128 + e.event.data1].emplace_back(t, e.event.data2);
                        break;
                    }
                    [[fallthrough]];
                case music::MidiEventType::NoteOff: {
                    auto& starts = open[ch * 128 + e.event.data1];
                    if (starts.empty()) break;
                    const auto [from, vel] = starts.front();
                    starts.erase(starts.begin());
                    part.notes.push_back(NoteEvent{from, std::max<Ticks>(1, t - from), e.event.data1, std::max(1, vel), ch, {}});
                    break;
                }
                case music::MidiEventType::ControlChange:
                    part.controllers.push_back(ControllerEvent{t, ch, e.event.data1, e.event.data2});
                    break;
                case music::MidiEventType::ProgramChange:
                    if (part.program < 0) part.program = e.event.data1;
                    break;
                default: break;
            }
        }
        // Notes never released end a quarter after they began.
        for (auto& [key, starts] : open)
            for (const auto& [from, vel] : starts)
                by_channel[key / 128].notes.push_back(NoteEvent{from, kTicksPerQuarter, key % 128, vel, key / 128, {}});
        int used = 0;
        for (const auto& [ch, part] : by_channel) used += part.notes.empty() ? 0 : 1;
        for (auto& [ch, part] : by_channel) {
            if (part.notes.empty()) continue;  // controllers alone make no part
            std::string name = track.name.empty() ? stem + (parts.empty() ? "" : " " + std::to_string(parts.size() + 1))
                                                  : track.name;
            if (used > 1) name += " (ch " + std::to_string(ch + 1) + ")";
            part.name = name;
            std::sort(part.notes.begin(), part.notes.end());
            std::stable_sort(part.controllers.begin(), part.controllers.end(),
                             [](const ControllerEvent& a, const ControllerEvent& b) { return a.tick < b.tick; });
            parts.push_back(std::move(part));
        }
    }
    if (parts.empty()) {
        result.error = "the file has no notes";
        return result;
    }

    const auto& d0 = data();
    const project::Track* target = d0.find_track(target_track);
    const bool target_ok = target && parts.size() == 1 &&
                           (target->kind == project::TrackKind::Instrument || target->kind == project::TrackKind::MidiOnly);
    // An empty project takes the file's tempo and meter.
    bool empty = true;
    for (const auto& t : d0.tracks)
        if (const auto* v = t.active_version(); v && (!v->regions.empty() || !v->audio_clips.empty() || v->score.total_note_count() > 0))
            empty = false;

    std::vector<std::string> new_ids, version_ids, region_ids;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        new_ids.push_back(project_.ids().next("trk"));
        version_ids.push_back(project_.ids().next("ver"));
        region_ids.push_back(project_.ids().next("rgn"));
    }
    const bool ok = edit("Import " + stem, [&](project::ProjectData& d) {
        if (empty && bpm >= 20.0 && bpm <= 400.0) {
            d.tempo.set(0, bpm);
            result.took_tempo = true;
        }
        if (empty && have_sig) {
            d.meter.set(1, sig);
            result.took_tempo = true;
        }
        const Ticks origin = d.meter.bar_start(d.meter.bar_at(std::max<Ticks>(0, at)));
        for (std::size_t i = 0; i < parts.size(); ++i) {
            Part& p = parts[i];
            project::Track* t = nullptr;
            if (target_ok) {
                t = d.find_track(target_track);
            } else {
                project::Track nt;
                nt.id = new_ids[i];
                nt.name = d.unique_track_name(p.name);
                nt.kind = project::TrackKind::Instrument;
                nt.instrument_standard_id = part_instrument(catalogs_.instruments, p);
                nt.include_in_export = true;
                nt.output_bus_id = "bus-master";
                project::TrackVersion v;
                v.id = version_ids[i];
                v.name = "Version 1";
                if (const auto* def = catalogs_.instruments.find(nt.instrument_standard_id)) {
                    nt.family = def->family;
                    nt.color = templates::family_color(def->family);
                    nt.assignment.status = project::ResolutionStatus::Placeholder;
                    nt.assignment.placeholder_program = std::max(0, def->gm_program);
                    nt.assignment.patch_display = "GM placeholder";
                    nt.assignment.message = "Built-in placeholder sound until a library patch or plug-in is assigned.";
                    if (def->unpitched) nt.midi_channel = 9;
                    v.score = templates::instrument_score(*def, nt.name);
                }
                nt.versions.push_back(std::move(v));
                nt.active_version_id = version_ids[i];
                d.tracks.push_back(std::move(nt));
                t = &d.tracks.back();
                result.new_tracks = true;
            }
            project::TrackVersion* v = t ? t->active_version() : nullptr;
            if (!v) return false;
            project::MidiRegion r;
            r.id = region_ids[i];
            r.source = "midi_file:" + stem;
            Ticks last = 0;
            for (auto& n : p.notes) {
                n.start += origin;
                n.channel = t->midi_channel;
                last = std::max(last, n.end());
            }
            for (auto& c : p.controllers) {
                c.tick += origin;
                c.channel = t->midi_channel;
            }
            r.start = origin;
            r.end = d.meter.bar_start(d.meter.bar_at(std::max(last, origin + 1) - 1) + 1);
            result.notes += static_cast<int>(p.notes.size());
            r.notes = std::move(p.notes);
            r.controllers = std::move(p.controllers);
            // Regions it overlaps on the main lane move to a take lane.
            int free_lane = 1;
            for (const auto& x : v->regions) free_lane = std::max(free_lane, x.lane + 1);
            for (auto& x : v->regions)
                if (x.lane == 0 && x.start < r.end && r.start < x.end) x.lane = free_lane;
            v->regions.push_back(std::move(r));
            result.track_ids.push_back(t->id);
            ++result.parts;
        }
        return true;
    });
    if (!ok) {
        result = MidiImport{};
        result.error = "the MIDI file could not be placed";
        return result;
    }
    if (!result.track_ids.empty()) select_track(result.track_ids.front());
    return result;
}

}  // namespace vcc::session
